#include "netlist_graph.h"

#include <json-glib/json-glib.h>
#include <string.h>

G_DEFINE_QUARK(netlist-graph-error-quark, netlist_graph_error)

typedef struct
{
    gint id;
    gchar *kind;
    gchar *path; // NULL for unnamed nodes
    NetlistLoc loc;
    GArray *in; // indexes into graph->edges
} Node;

typedef struct
{
    gint source;
    gint target;
    NetlistEdgeKind kind;
    gboolean disabled;
} Edge;

struct _NetlistGraph
{
    GPtrArray *files; // gchar*
    GArray *nodes; // Node
    GArray *edges; // Edge
    GHashTable *by_id; // gint -> index + 1 into nodes
    GHashTable *by_path; // gchar* -> index + 1 into nodes
    gchar *top;
};

void netlist_loc_free(NetlistLoc *loc)
{
    if (loc == NULL) {
        return;
    }
    g_free(loc->file);
    g_free(loc);
}

void netlist_source_free(NetlistSource *source)
{
    if (source == NULL) {
        return;
    }
    g_free(source->signal);
    if (source->statements != NULL) {
        g_ptr_array_unref(source->statements);
    }
    g_free(source);
}

static void node_clear(gpointer data)
{
    Node *n = data;
    g_free(n->kind);
    g_free(n->path);
    g_free(n->loc.file);
    g_array_unref(n->in);
}

static gint member_int(JsonObject *o, const gchar *name, gint fallback)
{
    return json_object_has_member(o, name) ? (gint)json_object_get_int_member(o, name) : fallback;
}

static const gchar *member_str(JsonObject *o, const gchar *name)
{
    if (!json_object_has_member(o, name)) {
        return NULL;
    }
    JsonNode *n = json_object_get_member(o, name);
    return JSON_NODE_HOLDS_VALUE(n) ? json_node_get_string(n) : NULL;
}

static void read_loc(NetlistGraph *g, JsonObject *o, NetlistLoc *loc)
{
    memset(loc, 0, sizeof *loc);
    if (!json_object_has_member(o, "location")) {
        return;
    }
    JsonObject *l = json_object_get_object_member(o, "location");
    gint file = member_int(l, "fileIndex", -1);
    if (file >= 0 && (guint)file < g->files->len) {
        loc->file = g_strdup(g_ptr_array_index(g->files, file));
    }
    loc->line = (guint)member_int(l, "line", 0);
    loc->column = (guint)member_int(l, "column", 0);
}

void netlist_graph_free(NetlistGraph *g)
{
    if (g == NULL) {
        return;
    }
    g_ptr_array_unref(g->files);
    g_array_unref(g->nodes);
    g_array_unref(g->edges);
    g_hash_table_unref(g->by_id);
    g_hash_table_unref(g->by_path);
    g_free(g->top);
    g_free(g);
}

NetlistGraph *netlist_graph_new(const gchar *json_text, GError **error)
{
    JsonParser *parser = json_parser_new();
    if (!json_parser_load_from_data(parser, json_text, -1, error)) {
        g_object_unref(parser);
        return NULL;
    }

    JsonNode *root = json_parser_get_root(parser);
    if (root == NULL || !JSON_NODE_HOLDS_OBJECT(root)) {
        g_set_error(error, NETLIST_GRAPH_ERROR, NETLIST_GRAPH_ERROR_PARSE,
                    "The netlist is not a JSON object.");
        g_object_unref(parser);
        return NULL;
    }
    JsonObject *obj = json_node_get_object(root);
    if (!json_object_has_member(obj, "nodes") || !json_object_has_member(obj, "edges")) {
        g_set_error(error, NETLIST_GRAPH_ERROR, NETLIST_GRAPH_ERROR_PARSE,
                    "The netlist has no nodes or edges.");
        g_object_unref(parser);
        return NULL;
    }

    NetlistGraph *g = g_new0(NetlistGraph, 1);
    g->files = g_ptr_array_new_with_free_func(g_free);
    g->nodes = g_array_new(FALSE, TRUE, sizeof(Node));
    g_array_set_clear_func(g->nodes, node_clear);
    g->edges = g_array_new(FALSE, TRUE, sizeof(Edge));
    g->by_id = g_hash_table_new(g_direct_hash, g_direct_equal);
    g->by_path = g_hash_table_new(g_str_hash, g_str_equal);

    if (json_object_has_member(obj, "fileTable")) {
        JsonArray *files = json_object_get_array_member(obj, "fileTable");
        for (guint i = 0; i < json_array_get_length(files); i++) {
            g_ptr_array_add(g->files, g_strdup(json_array_get_string_element(files, i)));
        }
    }

    JsonArray *nodes = json_object_get_array_member(obj, "nodes");
    for (guint i = 0; i < json_array_get_length(nodes); i++) {
        JsonObject *o = json_array_get_object_element(nodes, i);
        Node n = {0};
        n.id = member_int(o, "id", -1);
        n.kind = g_strdup(member_str(o, "kind") != NULL ? member_str(o, "kind") : "");
        n.path = g_strdup(member_str(o, "path"));
        n.in = g_array_new(FALSE, FALSE, sizeof(guint));
        read_loc(g, o, &n.loc);
        g_array_append_val(g->nodes, n);
    }
    for (guint i = 0; i < g->nodes->len; i++) {
        Node *n = &g_array_index(g->nodes, Node, i);
        g_hash_table_insert(g->by_id, GINT_TO_POINTER(n->id), GUINT_TO_POINTER(i + 1));
        if (n->path != NULL) {
            g_hash_table_insert(g->by_path, n->path, GUINT_TO_POINTER(i + 1));
            if (g->top == NULL) {
                const gchar *dot = strchr(n->path, '.');
                g->top = dot != NULL ? g_strndup(n->path, (gsize)(dot - n->path)) : g_strdup(n->path);
            }
        }
    }

    GHashTable *wires = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, (GDestroyNotify)g_array_unref);
    JsonArray *edges = json_object_get_array_member(obj, "edges");
    for (guint i = 0; i < json_array_get_length(edges); i++) {
        JsonObject *o = json_array_get_object_element(edges, i);
        Edge e = {0};
        e.source = member_int(o, "source", -1);
        e.target = member_int(o, "target", -1);
        const gchar *kind = member_str(o, "edgeKind");
        e.kind = kind == NULL ? NETLIST_EDGE_DATA
                 : g_str_equal(kind, "PosEdge") ? NETLIST_EDGE_POSEDGE
                 : g_str_equal(kind, "NegEdge") ? NETLIST_EDGE_NEGEDGE
                                                : NETLIST_EDGE_DATA;
        e.disabled = json_object_has_member(o, "disabled") && json_object_get_boolean_member(o, "disabled");
        gpointer t = g_hash_table_lookup(g->by_id, GINT_TO_POINTER(e.target));
        if (t == NULL || g_hash_table_lookup(g->by_id, GINT_TO_POINTER(e.source)) == NULL) {
            continue;
        }

        // A plain wire joining two instances has no node of its own; the
        // edges it rides on name it (`symbol.path`). Remember its driver.
        if (json_object_has_member(o, "symbol")) {
            JsonObject *sym = json_object_get_object_member(o, "symbol");
            const gchar *wire = member_str(sym, "path");
            if (wire != NULL && !g_hash_table_contains(g->by_path, wire)) {
                GArray *drivers = g_hash_table_lookup(wires, wire);
                if (drivers == NULL) {
                    drivers = g_array_new(FALSE, FALSE, sizeof(gint));
                    g_hash_table_insert(wires, g_strdup(wire), drivers);
                }
                gboolean known = FALSE;
                for (guint k = 0; k < drivers->len && !known; k++) {
                    known = g_array_index(drivers, gint, k) == e.source;
                }
                if (!known) {
                    g_array_append_val(drivers, e.source);
                }
            }
        }
        g_array_append_val(g->edges, e);
        guint idx = g->edges->len - 1;
        Node *target = &g_array_index(g->nodes, Node, GPOINTER_TO_UINT(t) - 1);
        g_array_append_val(target->in, idx);
    }

    // Give each wire a node fed by its drivers, so it can be traced and
    // named like any signal.
    GHashTableIter wit;
    gpointer wkey, wval;
    gint next_id = -1;
    g_hash_table_iter_init(&wit, wires);
    while (g_hash_table_iter_next(&wit, &wkey, &wval)) {
        const GArray *drivers = wval;
        Node w = {0};
        w.id = next_id--;
        w.kind = g_strdup("Wire");
        w.path = g_strdup(wkey);
        w.in = g_array_new(FALSE, FALSE, sizeof(guint));
        for (guint k = 0; k < drivers->len; k++) {
            Edge e = {0};
            e.source = g_array_index(drivers, gint, k);
            e.target = w.id;
            e.kind = NETLIST_EDGE_DATA;
            g_array_append_val(g->edges, e);
            guint idx = g->edges->len - 1;
            g_array_append_val(w.in, idx);
        }
        g_array_append_val(g->nodes, w);
        guint pos = g->nodes->len;
        g_hash_table_insert(g->by_id, GINT_TO_POINTER(w.id), GUINT_TO_POINTER(pos));
        g_hash_table_insert(g->by_path, ((Node *)g->nodes->data)[pos - 1].path, GUINT_TO_POINTER(pos));
    }
    g_hash_table_unref(wires);

    g_object_unref(parser);
    if (g->top == NULL) {
        netlist_graph_free(g);
        g_set_error(error, NETLIST_GRAPH_ERROR, NETLIST_GRAPH_ERROR_PARSE,
                    "The netlist has no named signals.");
        return NULL;
    }
    return g;
}

const gchar *netlist_graph_top(const NetlistGraph *g)
{
    return g->top;
}

static Node *node_by_path(const NetlistGraph *g, const gchar *path)
{
    gpointer v = g_hash_table_lookup(g->by_path, path);
    return v != NULL ? &g_array_index(g->nodes, Node, GPOINTER_TO_UINT(v) - 1) : NULL;
}

static Node *node_by_id(const NetlistGraph *g, gint id)
{
    gpointer v = g_hash_table_lookup(g->by_id, GINT_TO_POINTER(id));
    return v != NULL ? &g_array_index(g->nodes, Node, GPOINTER_TO_UINT(v) - 1) : NULL;
}

gboolean netlist_graph_has_signal(const NetlistGraph *g, const gchar *path)
{
    return node_by_path(g, path) != NULL;
}

gboolean netlist_graph_is_register(const NetlistGraph *g, const gchar *path)
{
    Node *n = node_by_path(g, path);
    return n != NULL && g_str_equal(n->kind, "State");
}

static NetlistLoc *loc_copy(const NetlistLoc *l)
{
    NetlistLoc *c = g_new0(NetlistLoc, 1);
    c->file = g_strdup(l->file);
    c->line = l->line;
    c->column = l->column;
    return c;
}

gboolean netlist_graph_declaration(const NetlistGraph *g, const gchar *path, NetlistLoc *out)
{
    Node *n = node_by_path(g, path);
    if (n == NULL) {
        return FALSE;
    }
    out->file = g_strdup(n->loc.file);
    out->line = n->loc.line;
    out->column = n->loc.column;
    return TRUE;
}

static gboolean is_statement(const Node *n)
{
    return g_str_equal(n->kind, "Assignment") || g_str_equal(n->kind, "Case") ||
           g_str_equal(n->kind, "Conditional");
}

// Walks backwards from `start` through unnamed nodes, calling `found` for
// every named node reached (with the edge into the walk's first hop, and
// the statements crossed) -- iterative so a deep design can't blow the stack.
typedef struct
{
    const Node *node;
    GPtrArray *statements; // owned NetlistLoc*, nearest the target first
} Frame;

static void frame_free(Frame *f)
{
    g_ptr_array_unref(f->statements);
}

GPtrArray *netlist_graph_sources(const NetlistGraph *g, const gchar *path)
{
    const Node *target = node_by_path(g, path);
    if (target == NULL) {
        return NULL;
    }

    GPtrArray *out = g_ptr_array_new_with_free_func((GDestroyNotify)netlist_source_free);
    GHashTable *seen = g_hash_table_new(g_direct_hash, g_direct_equal); // unnamed node ids
    GArray *stack = g_array_new(FALSE, FALSE, sizeof(Frame));

    Frame first = {target, g_ptr_array_new_with_free_func((GDestroyNotify)netlist_loc_free)};
    g_array_append_val(stack, first);

    while (stack->len > 0) {
        Frame f = g_array_index(stack, Frame, stack->len - 1);
        g_array_set_size(stack, stack->len - 1); // no clear func: we own f now

        for (guint i = 0; i < f.node->in->len; i++) {
            const Edge *e = &g_array_index(g->edges, Edge, g_array_index(f.node->in, guint, i));
            if (e->disabled) {
                continue;
            }
            const Node *src = node_by_id(g, e->source);
            if (src == NULL) {
                continue;
            }

            GPtrArray *stmts = g_ptr_array_new_with_free_func((GDestroyNotify)netlist_loc_free);
            for (guint k = 0; k < f.statements->len; k++) {
                g_ptr_array_add(stmts, loc_copy(g_ptr_array_index(f.statements, k)));
            }
            if (is_statement(src) && src->loc.line != 0) {
                g_ptr_array_add(stmts, loc_copy(&src->loc));
            }

            if (src->path != NULL) {
                NetlistSource *s = NULL;
                for (guint k = 0; k < out->len; k++) {
                    NetlistSource *c = g_ptr_array_index(out, k);
                    if (c->kind == e->kind && g_str_equal(c->signal, src->path)) {
                        s = c;
                        break;
                    }
                }
                if (s == NULL) {
                    s = g_new0(NetlistSource, 1);
                    s->signal = g_strdup(src->path);
                    s->kind = e->kind;
                    s->statements = g_ptr_array_new_with_free_func((GDestroyNotify)netlist_loc_free);
                    g_ptr_array_add(out, s);
                }
                for (guint k = 0; k < stmts->len; k++) {
                    const NetlistLoc *l = g_ptr_array_index(stmts, k);
                    gboolean dup = FALSE;
                    for (guint m = 0; m < s->statements->len && !dup; m++) {
                        const NetlistLoc *x = g_ptr_array_index(s->statements, m);
                        dup = x->line == l->line && x->column == l->column &&
                              g_strcmp0(x->file, l->file) == 0;
                    }
                    if (!dup) {
                        g_ptr_array_add(s->statements, loc_copy(l));
                    }
                }
                g_ptr_array_unref(stmts);
            } else if (!g_hash_table_contains(seen, GINT_TO_POINTER(src->id))) {
                g_hash_table_add(seen, GINT_TO_POINTER(src->id));
                Frame next = {src, stmts};
                g_array_append_val(stack, next);
            } else {
                g_ptr_array_unref(stmts);
            }
        }
        frame_free(&f);
    }

    g_array_unref(stack);
    g_hash_table_unref(seen);
    return out;
}

static gboolean loc_listed(GPtrArray *list, const NetlistLoc *l)
{
    for (guint i = 0; i < list->len; i++) {
        const NetlistLoc *x = g_ptr_array_index(list, i);
        if (x->line == l->line && x->column == l->column && g_strcmp0(x->file, l->file) == 0) {
            return TRUE;
        }
    }
    return FALSE;
}

GPtrArray *netlist_graph_driver_statements(const NetlistGraph *g, const gchar *path)
{
    const Node *target = node_by_path(g, path);
    GPtrArray *out = g_ptr_array_new_with_free_func((GDestroyNotify)netlist_loc_free);
    if (target == NULL) {
        return out;
    }

    GHashTable *seen = g_hash_table_new(g_direct_hash, g_direct_equal);
    GPtrArray *stack = g_ptr_array_new();
    g_ptr_array_add(stack, (gpointer)target);
    while (stack->len > 0) {
        const Node *n = g_ptr_array_remove_index(stack, stack->len - 1);
        for (guint i = 0; i < n->in->len; i++) {
            const Edge *e = &g_array_index(g->edges, Edge, g_array_index(n->in, guint, i));
            const Node *src = node_by_id(g, e->source);
            if (e->disabled || e->kind != NETLIST_EDGE_DATA || src == NULL) {
                continue;
            }
            // Only the assignments that write this signal, not the logic
            // that computes their operands: stop at the first statement.
            if (g_str_equal(src->kind, "Assignment")) {
                if (src->loc.line != 0 && !loc_listed(out, &src->loc)) {
                    g_ptr_array_add(out, loc_copy(&src->loc));
                }
            } else if (src->path == NULL && !g_hash_table_contains(seen, GINT_TO_POINTER(src->id))) {
                g_hash_table_add(seen, GINT_TO_POINTER(src->id));
                g_ptr_array_add(stack, (gpointer)src);
            }
        }
    }
    g_ptr_array_unref(stack);
    g_hash_table_unref(seen);
    return out;
}

static gboolean file_matches(const gchar *node_file, const gchar *wanted)
{
    if (node_file == NULL || wanted == NULL) {
        return FALSE;
    }
    if (g_str_equal(node_file, wanted)) {
        return TRUE;
    }
    gchar *a = g_path_get_basename(node_file);
    gchar *b = g_path_get_basename(wanted);
    gboolean same = g_str_equal(a, b) &&
                    (g_str_has_suffix(node_file, wanted) || g_str_has_suffix(wanted, node_file));
    g_free(a);
    g_free(b);
    return same;
}

static gint cmp_str(gconstpointer a, gconstpointer b)
{
    return g_strcmp0(*(const gchar *const *)a, *(const gchar *const *)b);
}

GPtrArray *netlist_graph_signals_at_line(const NetlistGraph *g, const gchar *file, guint line)
{
    GPtrArray *out = g_ptr_array_new_with_free_func(g_free);
    GHashTable *added = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);

    for (guint i = 0; i < g->nodes->len; i++) {
        const Node *n = &g_array_index(g->nodes, Node, i);
        if (n->loc.line != line || !file_matches(n->loc.file, file)) {
            continue;
        }
        if (n->path != NULL) {
            if (!g_hash_table_contains(added, n->path)) {
                gchar *p = g_strdup(n->path);
                g_hash_table_add(added, p);
                g_ptr_array_add(out, g_strdup(p));
            }
            continue;
        }
        // A statement: the signals it writes are its named successors.
        for (guint k = 0; k < g->edges->len; k++) {
            const Edge *e = &g_array_index(g->edges, Edge, k);
            if (e->source != n->id || e->disabled) {
                continue;
            }
            const Node *dst = node_by_id(g, e->target);
            // Follow merges to the signal they join.
            for (guint hop = 0; dst != NULL && dst->path == NULL && hop < 8; hop++) {
                const Node *next = NULL;
                for (guint m = 0; m < g->edges->len && next == NULL; m++) {
                    const Edge *e2 = &g_array_index(g->edges, Edge, m);
                    if (e2->source == dst->id && !e2->disabled) {
                        next = node_by_id(g, e2->target);
                    }
                }
                dst = next;
            }
            if (dst != NULL && dst->path != NULL && !g_hash_table_contains(added, dst->path)) {
                gchar *p = g_strdup(dst->path);
                g_hash_table_add(added, p);
                g_ptr_array_add(out, g_strdup(p));
            }
        }
    }

    g_ptr_array_sort(out, cmp_str);
    g_hash_table_unref(added);
    return out;
}

/* ---------------------------------------------- dump <-> netlist names -- */

static gchar *strip_bit_range(const gchar *name)
{
    gsize len = strlen(name);
    while (len > 0 && name[len - 1] == ' ') {
        len--;
    }
    if (len > 0 && name[len - 1] == ']') {
        const gchar *open = g_strrstr_len(name, (gssize)len, "[");
        if (open != NULL) {
            len = (gsize)(open - name);
            while (len > 0 && name[len - 1] == ' ') {
                len--;
            }
        }
    }
    return g_strndup(name, len);
}

gchar *netlist_graph_resolve_dump_path(const NetlistGraph *g, const gchar *dump_path, gchar **dump_prefix)
{
    gchar *clean = strip_bit_range(dump_path);
    gchar **parts = g_strsplit(clean, ".", -1);
    guint n = g_strv_length(parts);
    gchar *found = NULL;

    for (guint k = 1; k < n && found == NULL; k++) {
        gchar *rest = g_strjoinv(".", parts + k);
        gchar *candidate = g_strconcat(g->top, ".", rest, NULL);
        g_free(rest);
        if (node_by_path(g, candidate) != NULL) {
            found = candidate;
            if (dump_prefix != NULL) {
                gchar *saved = parts[k];
                parts[k] = NULL;
                *dump_prefix = g_strjoinv(".", parts);
                parts[k] = saved;
            }
        } else {
            g_free(candidate);
        }
    }

    g_strfreev(parts);
    g_free(clean);
    return found;
}

gchar *netlist_graph_to_dump_path(const NetlistGraph *g, const gchar *dump_prefix, const gchar *netlist_path)
{
    gsize top_len = strlen(g->top);
    if (!g_str_has_prefix(netlist_path, g->top) ||
        (netlist_path[top_len] != '.' && netlist_path[top_len] != '\0')) {
        return NULL;
    }
    return g_strconcat(dump_prefix, netlist_path + top_len, NULL);
}

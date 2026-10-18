#include "schematic_node_key.h"

#include <string.h>

#define MAX_ROUNDS 12

typedef struct
{
    gchar *id;
    gchar *label;
    gchar *color; // current colour, hex digest
    GPtrArray *out; // Edge*, edges leaving this node
    GPtrArray *in; // Edge*, edges entering it
} Node;

typedef struct
{
    Node *from;
    Node *to;
    gchar *label;
} Edge;

static void node_free(gpointer data)
{
    Node *n = data;
    g_free(n->id);
    g_free(n->label);
    g_free(n->color);
    g_ptr_array_unref(n->out);
    g_ptr_array_unref(n->in);
    g_free(n);
}

static void edge_free(gpointer data)
{
    Edge *e = data;
    g_free(e->label);
    g_free(e);
}

static gchar *digest(const gchar *text)
{
    return g_compute_checksum_for_string(G_CHECKSUM_SHA1, text, -1);
}

static gint cmp_str(gconstpointer a, gconstpointer b)
{
    return g_strcmp0(*(gchar *const *)a, *(gchar *const *)b);
}

// A quoted DOT string's content, with backslash escapes left as written
// (they're only ever compared with themselves).
static gchar *unquote(const gchar *s)
{
    return g_strdup(s);
}

static gchar *colour_round(Node *n)
{
    GPtrArray *parts = g_ptr_array_new_with_free_func(g_free);

    for (guint i = 0; i < n->out->len; i++) {
        const Edge *e = g_ptr_array_index(n->out, i);
        g_ptr_array_add(parts, g_strdup_printf(">|%s|%s", e->label, e->to->color));
    }
    for (guint i = 0; i < n->in->len; i++) {
        const Edge *e = g_ptr_array_index(n->in, i);
        g_ptr_array_add(parts, g_strdup_printf("<|%s|%s", e->label, e->from->color));
    }
    g_ptr_array_sort(parts, cmp_str);

    GString *buf = g_string_new(n->color);
    for (guint i = 0; i < parts->len; i++) {
        g_string_append_c(buf, '\n');
        g_string_append(buf, g_ptr_array_index(parts, i));
    }
    gchar *d = digest(buf->str);
    g_string_free(buf, TRUE);
    g_ptr_array_unref(parts);
    return d;
}

static guint distinct_colours(GPtrArray *nodes)
{
    GHashTable *seen = g_hash_table_new(g_str_hash, g_str_equal);
    for (guint i = 0; i < nodes->len; i++) {
        g_hash_table_add(seen, ((Node *)g_ptr_array_index(nodes, i))->color);
    }
    guint n = g_hash_table_size(seen);
    g_hash_table_unref(seen);
    return n;
}

typedef struct
{
    gchar *from, *to, *label;
} RawEdge;

static void raw_edge_free(gpointer data)
{
    RawEdge *e = data;
    g_free(e->from);
    g_free(e->to);
    g_free(e->label);
    g_free(e);
}

GHashTable *schematic_node_keys(const gchar *dot)
{
    GHashTable *keys = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
    if (dot == NULL) {
        return keys;
    }

    GHashTable *by_id = g_hash_table_new(g_str_hash, g_str_equal);
    GPtrArray *nodes = g_ptr_array_new_with_free_func(node_free);
    GPtrArray *edges = g_ptr_array_new_with_free_func(edge_free);
    GPtrArray *raw_edges = g_ptr_array_new_with_free_func(raw_edge_free);

    GRegex *node_re = g_regex_new("^\\s*(N[0-9]+)\\s*\\[label=\"((?:[^\"\\\\]|\\\\.)*)\"", 0, 0, NULL);
    GRegex *edge_re = g_regex_new(
        "^\\s*(N[0-9]+)\\s*->\\s*(N[0-9]+)\\s*(?:\\[label=\"((?:[^\"\\\\]|\\\\.)*)\")?", 0, 0, NULL);

    gchar **lines = g_strsplit(dot, "\n", -1);
    for (guint i = 0; lines[i] != NULL; i++) {
        GMatchInfo *mi = NULL;
        if (g_regex_match(edge_re, lines[i], 0, &mi)) {
            RawEdge *e = g_new0(RawEdge, 1);
            e->from = g_match_info_fetch(mi, 1);
            e->to = g_match_info_fetch(mi, 2);
            gchar *label = g_match_info_fetch(mi, 3);
            e->label = unquote(label != NULL ? label : "");
            g_free(label);
            g_ptr_array_add(raw_edges, e);
        }
        g_match_info_free(mi);

        mi = NULL;
        if (g_regex_match(node_re, lines[i], 0, &mi)) {
            Node *n = g_new0(Node, 1);
            n->id = g_match_info_fetch(mi, 1);
            gchar *label = g_match_info_fetch(mi, 2);
            n->label = unquote(label);
            g_free(label);
            n->out = g_ptr_array_new();
            n->in = g_ptr_array_new();
            n->color = digest(n->label);
            g_ptr_array_add(nodes, n);
            g_hash_table_insert(by_id, n->id, n);
        }
        g_match_info_free(mi);
    }
    g_strfreev(lines);
    g_regex_unref(node_re);
    g_regex_unref(edge_re);

    // Edges naming a node we never saw declared are ignored.
    for (guint i = 0; i < raw_edges->len; i++) {
        const RawEdge *re = g_ptr_array_index(raw_edges, i);
        Node *from = g_hash_table_lookup(by_id, re->from);
        Node *to = g_hash_table_lookup(by_id, re->to);
        if (from == NULL || to == NULL) {
            continue;
        }
        Edge *e = g_new0(Edge, 1);
        e->from = from;
        e->to = to;
        e->label = g_strdup(re->label);
        g_ptr_array_add(edges, e);
        g_ptr_array_add(from->out, e);
        g_ptr_array_add(to->in, e);
    }
    g_ptr_array_unref(raw_edges);

    // Refine until the partition stops getting finer.
    guint distinct = distinct_colours(nodes);
    for (guint round = 0; round < MAX_ROUNDS; round++) {
        gchar **next = g_new0(gchar *, nodes->len);
        for (guint i = 0; i < nodes->len; i++) {
            next[i] = colour_round(g_ptr_array_index(nodes, i));
        }
        for (guint i = 0; i < nodes->len; i++) {
            Node *n = g_ptr_array_index(nodes, i);
            g_free(n->color);
            n->color = next[i];
        }
        g_free(next);

        guint now = distinct_colours(nodes);
        if (now == distinct) {
            break;
        }
        distinct = now;
    }

    for (guint i = 0; i < nodes->len; i++) {
        Node *n = g_ptr_array_index(nodes, i);
        g_hash_table_insert(keys, g_strdup(n->id), g_strndup(n->color, 16));
    }

    g_hash_table_unref(by_id);
    g_ptr_array_unref(edges);
    g_ptr_array_unref(nodes);
    return keys;
}

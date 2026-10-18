#include "schematic_layout.h"

#include <string.h>

typedef struct
{
    gchar *label;
    gdouble dx, dy;
} Entry;

static void entry_free(gpointer data)
{
    Entry *e = data;
    g_free(e->label);
    g_free(e);
}

// root path -> (node id -> Entry*)
static GHashTable *store = NULL;

static GHashTable *roots(void)
{
    if (store == NULL) {
        store = g_hash_table_new_full(g_str_hash, g_str_equal, g_free,
                                      (GDestroyNotify)g_hash_table_unref);
    }
    return store;
}

void schematic_layout_set(const gchar *root,
                          const gchar *node_id,
                          const gchar *label,
                          gdouble dx,
                          gdouble dy)
{
    GHashTable *nodes = g_hash_table_lookup(roots(), root);

    // 0,0 means "back where Graphviz put it": drop the entry rather than
    // store a no-op that would keep the save file and the "has overrides"
    // check (which switches the view to the SVG renderer) alive forever.
    if (dx == 0.0 && dy == 0.0) {
        if (nodes != NULL) {
            g_hash_table_remove(nodes, node_id);
            if (g_hash_table_size(nodes) == 0) {
                g_hash_table_remove(roots(), root);
            }
        }
        return;
    }

    if (nodes == NULL) {
        nodes = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, entry_free);
        g_hash_table_insert(roots(), g_strdup(root), nodes);
    }
    Entry *e = g_new0(Entry, 1);
    e->label = g_strdup(label);
    e->dx = dx;
    e->dy = dy;
    g_hash_table_insert(nodes, g_strdup(node_id), e);
}

gboolean schematic_layout_get(const gchar *root,
                              const gchar *node_id,
                              const gchar *label,
                              gdouble *dx,
                              gdouble *dy)
{
    GHashTable *nodes = g_hash_table_lookup(roots(), root);
    if (nodes == NULL) {
        return FALSE;
    }
    const Entry *e = g_hash_table_lookup(nodes, node_id);
    if (e == NULL || g_strcmp0(e->label, label) != 0) {
        return FALSE;
    }
    *dx = e->dx;
    *dy = e->dy;
    return TRUE;
}

guint schematic_layout_count(const gchar *root)
{
    GHashTable *nodes = g_hash_table_lookup(roots(), root);
    return nodes != NULL ? g_hash_table_size(nodes) : 0;
}

void schematic_layout_clear(const gchar *root)
{
    g_hash_table_remove(roots(), root);
}

void schematic_layout_clear_all(void)
{
    g_hash_table_remove_all(roots());
}

static void write_quoted(FILE *out, const gchar *s)
{
    gchar *esc = g_strescape(s, NULL);
    fprintf(out, " \"%s\"", esc);
    g_free(esc);
}

void schematic_layout_write(FILE *out)
{
    GHashTableIter ri;
    gpointer rk, rv;
    g_hash_table_iter_init(&ri, roots());
    while (g_hash_table_iter_next(&ri, &rk, &rv)) {
        GHashTableIter ni;
        gpointer nk, nv;
        g_hash_table_iter_init(&ni, rv);
        while (g_hash_table_iter_next(&ni, &nk, &nv)) {
            const Entry *e = nv;
            gchar a[G_ASCII_DTOSTR_BUF_SIZE], b[G_ASCII_DTOSTR_BUF_SIZE];
            g_ascii_formatd(a, sizeof(a), "%.2f", e->dx);
            g_ascii_formatd(b, sizeof(b), "%.2f", e->dy);
            fprintf(out, "[schematic_offset] %s %s", a, b);
            write_quoted(out, rk);
            write_quoted(out, nk);
            write_quoted(out, e->label);
            fputc('\n', out);
        }
    }
}

// Reads one "..." token (g_strescape-escaped) starting at *p, advancing it.
static gchar *read_quoted(const gchar **p)
{
    const gchar *s = *p;
    while (*s == ' ') {
        s++;
    }
    if (*s != '"') {
        return NULL;
    }
    s++;
    const gchar *start = s;
    while (*s != '\0' && *s != '"') {
        if (*s == '\\' && s[1] != '\0') {
            s++;
        }
        s++;
    }
    if (*s != '"') {
        return NULL;
    }
    gchar *raw = g_strndup(start, (gsize)(s - start));
    gchar *out = g_strcompress(raw);
    g_free(raw);
    *p = s + 1;
    return out;
}

gboolean schematic_layout_read(const gchar *args)
{
    gchar *end = NULL;
    const gchar *p = args;
    while (*p == ' ') {
        p++;
    }
    gdouble dx = g_ascii_strtod(p, &end);
    if (end == p) {
        return FALSE;
    }
    p = end;
    gdouble dy = g_ascii_strtod(p, &end);
    if (end == p) {
        return FALSE;
    }
    p = end;

    gchar *root = read_quoted(&p);
    gchar *id = root != NULL ? read_quoted(&p) : NULL;
    gchar *label = id != NULL ? read_quoted(&p) : NULL;

    gboolean ok = label != NULL;
    if (ok) {
        schematic_layout_set(root, id, label, dx, dy);
    }
    g_free(root);
    g_free(id);
    g_free(label);
    return ok;
}

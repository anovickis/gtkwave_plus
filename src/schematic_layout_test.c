#include <glib/gstdio.h>
#include <unistd.h>
#include <glib.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "schematic_layout.h"
#include "schematic_svg_edit.h"

/* ------------------------------------------------------ layout store -- */

static void test_store_basics(void)
{
    schematic_layout_clear_all();
    gdouble dx = 0, dy = 0;

    g_assert_false(schematic_layout_get("top.x", "N1", "Assignment", &dx, &dy));
    g_assert_cmpuint(schematic_layout_count("top.x"), ==, 0);

    schematic_layout_set("top.x", "N1", "Assignment", 12.5, -3.0);
    g_assert_cmpuint(schematic_layout_count("top.x"), ==, 1);
    g_assert_true(schematic_layout_get("top.x", "N1", "Assignment", &dx, &dy));
    g_assert_cmpfloat(dx, ==, 12.5);
    g_assert_cmpfloat(dy, ==, -3.0);

    // Different root, and a stale label (renumbered design): not applied.
    g_assert_false(schematic_layout_get("top.y", "N1", "Assignment", &dx, &dy));
    g_assert_false(schematic_layout_get("top.x", "N1", "Conditional", &dx, &dy));

    // Moving a node back to 0,0 removes the entry (and the empty root).
    schematic_layout_set("top.x", "N1", "Assignment", 0.0, 0.0);
    g_assert_cmpuint(schematic_layout_count("top.x"), ==, 0);

    schematic_layout_set("top.x", "N1", "a", 1, 1);
    schematic_layout_set("top.x", "N2", "b", 2, 2);
    schematic_layout_set("top.y", "N1", "c", 3, 3);
    schematic_layout_clear("top.x");
    g_assert_cmpuint(schematic_layout_count("top.x"), ==, 0);
    g_assert_cmpuint(schematic_layout_count("top.y"), ==, 1);
    schematic_layout_clear_all();
    g_assert_cmpuint(schematic_layout_count("top.y"), ==, 0);
}

static void test_store_roundtrip(void)
{
    schematic_layout_clear_all();
    // Labels with quotes, backslashes, brackets, spaces -- everything a
    // real label (`count_q [3:0]`) or hostile path could contain.
    schematic_layout_set("top.u \"q\"", "N22", "count_q [3:0]", 10.25, -4.5);
    schematic_layout_set("top.u \"q\"", "N7", "a\\b \"c\"", -1.5, 99.0);
    schematic_layout_set("top.other", "N1", "Assignment", 5, 6);

    gchar *path = NULL;
    gint fd = g_file_open_tmp("layout-XXXXXX", &path, NULL);
    g_assert_cmpint(fd, >=, 0);
    FILE *f = fdopen(fd, "w");
    schematic_layout_write(f);
    fclose(f);

    gchar *text = NULL;
    g_assert_true(g_file_get_contents(path, &text, NULL, NULL));
    schematic_layout_clear_all();

    gchar **lines = g_strsplit(text, "\n", -1);
    guint parsed = 0;
    for (guint i = 0; lines[i] != NULL; i++) {
        if (lines[i][0] == '\0') {
            continue;
        }
        g_assert_true(g_str_has_prefix(lines[i], "[schematic_offset] "));
        g_assert_true(schematic_layout_read(lines[i] + strlen("[schematic_offset] ")));
        parsed++;
    }
    g_assert_cmpuint(parsed, ==, 3);

    gdouble dx, dy;
    g_assert_true(schematic_layout_get("top.u \"q\"", "N22", "count_q [3:0]", &dx, &dy));
    g_assert_cmpfloat(dx, ==, 10.25);
    g_assert_cmpfloat(dy, ==, -4.5);
    g_assert_true(schematic_layout_get("top.u \"q\"", "N7", "a\\b \"c\"", &dx, &dy));
    g_assert_cmpfloat(dx, ==, -1.5);
    g_assert_cmpfloat(dy, ==, 99.0);
    g_assert_true(schematic_layout_get("top.other", "N1", "Assignment", &dx, &dy));

    g_strfreev(lines);
    g_free(text);
    g_unlink(path);
    g_free(path);
    schematic_layout_clear_all();
}

static void test_store_rejects_malformed(void)
{
    schematic_layout_clear_all();
    g_assert_false(schematic_layout_read(""));
    g_assert_false(schematic_layout_read("abc 1 \"r\" \"i\" \"l\""));
    g_assert_false(schematic_layout_read("1 2 \"r\" \"i\""));
    g_assert_false(schematic_layout_read("1 2 \"r\" \"i\" \"unterminated"));
    g_assert_cmpuint(schematic_layout_count("r"), ==, 0);
}

/* ---------------------------------------------------------- svg edit -- */

static const gchar *TEST_DOT =
    "digraph {\n"
    "  node [shape=record];\n"
    "  N1 [label=\"In port a\"]\n"
    "  N2 [label=\"In port b\"]\n"
    "  N3 [label=\"Assignment\"]\n"
    "  N4 [label=\"y [3:0]\"]\n"
    "  N5 [label=\"Unrelated\"]\n"
    "  N6 [label=\"Unrelated 2\"]\n"
    "  N1 -> N3 [label=\"a[0]\"]\n"
    "  N2 -> N3 [label=\"b[0]\"]\n"
    "  N3 -> N4 [label=\"y[0]\"]\n"
    "  N4 -> N3 [label=\"fb[0]\"]\n"
    "  N5 -> N6 [label=\"z[0]\"]\n"
    "}\n";

static gboolean run_dot(const gchar *mode, gchar **out)
{
    gchar *dot = g_find_program_in_path("dot");
    if (dot == NULL) {
        return FALSE;
    }
    gchar *dot_path = NULL;
    gint fd = g_file_open_tmp("svgedit-XXXXXX.dot", &dot_path, NULL);
    close(fd);
    g_file_set_contents(dot_path, TEST_DOT, -1, NULL);
    gchar *argv[] = {dot, (gchar *)"-Gpad=0", (gchar *)"-Gmargin=0", (gchar *)mode, dot_path, NULL};
    gint status = 0;
    gboolean ok = g_spawn_sync(NULL, argv, NULL, G_SPAWN_DEFAULT, NULL, NULL, out, NULL, &status, NULL);
    g_unlink(dot_path);
    g_free(dot_path);
    g_free(dot);
    return ok && status == 0;
}

typedef struct
{
    GHashTable *nodes;
    gdouble w, h;
    gchar *svg;
} Fixture;

static gboolean fixture_load(Fixture *fx)
{
    gchar *plain = NULL;
    if (!run_dot("-Tplain", &plain) || !run_dot("-Tsvg", &fx->svg)) {
        g_free(plain);
        return FALSE;
    }
    fx->nodes = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
    gchar **lines = g_strsplit(plain, "\n", -1);
    for (guint i = 0; lines[i] != NULL; i++) {
        gchar name[64];
        gdouble a, b, c, d;
        if (sscanf(lines[i], "graph %*f %lf %lf", &a, &b) == 2) {
            fx->w = a * 72.0;
            fx->h = b * 72.0;
        } else if (sscanf(lines[i], "node %63s %lf %lf %lf %lf", name, &a, &b, &c, &d) == 5) {
            SchematicSvgNode *n = g_new0(SchematicSvgNode, 1);
            n->cx = a * 72.0;
            n->cy = -b * 72.0;
            n->w = c * 72.0;
            n->h = d * 72.0;
            g_hash_table_insert(fx->nodes, g_strdup(name), n);
        }
    }
    g_strfreev(lines);
    g_free(plain);
    return TRUE;
}

static void fixture_free(Fixture *fx)
{
    g_hash_table_unref(fx->nodes);
    g_free(fx->svg);
}

static GHashTable *offsets_with(const gchar *id, gdouble dx, gdouble dy)
{
    GHashTable *o = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
    SchematicSvgOffset *off = g_new0(SchematicSvgOffset, 1);
    off->dx = dx;
    off->dy = dy;
    g_hash_table_insert(o, g_strdup(id), off);
    return o;
}

// Returns the text of the group whose <title> is `title`, or NULL.
static gchar *find_group(const gchar *svg, const gchar *title)
{
    gchar *needle = g_strdup_printf("<title>%s</title>", title);
    const gchar *t = strstr(svg, needle);
    g_free(needle);
    if (t == NULL) {
        return NULL;
    }
    const gchar *start = t;
    while (start > svg && strncmp(start, "<g id=", 6) != 0) {
        start--;
    }
    const gchar *end = strstr(t, "</g>");
    return g_strndup(start, (gsize)(end - start) + 4);
}

// Midpoint (t = 0.5) of the edge's single cubic "M x,y C a,b c,d e,f".
static gboolean path_midpoint(const gchar *group, gdouble *mx, gdouble *my)
{
    const gchar *d = strstr(group, " d=\"M");
    if (d == NULL) {
        return FALSE;
    }
    gdouble v[8];
    const gchar *p = d + 5;
    guint n = 0;
    while (n < 8 && *p != '"' && *p != '\0') {
        if (g_ascii_isdigit(*p) || *p == '-' || *p == '.') {
            gchar *end;
            v[n++] = g_ascii_strtod(p, &end);
            p = end;
        } else {
            p++;
        }
    }
    if (n < 8) {
        return FALSE;
    }
    *mx = (v[0] + 3 * v[2] + 3 * v[4] + v[6]) / 8.0;
    *my = (v[1] + 3 * v[3] + 3 * v[5] + v[7]) / 8.0;
    return TRUE;
}

static gboolean well_formed(const gchar *svg)
{
    GMarkupParser parser = {0};
    GMarkupParseContext *ctx = g_markup_parse_context_new(&parser, 0, NULL, NULL);
    GError *err = NULL;
    gboolean ok = g_markup_parse_context_parse(ctx, svg, -1, &err) &&
                  g_markup_parse_context_end_parse(ctx, &err);
    if (!ok) {
        g_printerr("not well-formed: %s\n", err->message);
        g_clear_error(&err);
    }
    g_markup_parse_context_free(ctx);
    return ok;
}

static void test_svg_move_node(void)
{
    Fixture fx = {0};
    if (!fixture_load(&fx)) {
        g_test_skip("graphviz 'dot' not available");
        return;
    }

    GHashTable *offsets = offsets_with("N3", 140.0, 60.0);
    SchematicSvgBounds b;
    GError *err = NULL;
    gchar *out = schematic_svg_apply_offsets(fx.svg, fx.nodes, offsets, fx.w, fx.h,
                                             96.0 / 72.0, &b, &err);
    g_assert_no_error(err);
    g_assert_nonnull(out);
    g_assert_true(well_formed(out));

    // The moved node's group carries the transform; others don't.
    gchar *n3 = find_group(out, "N3");
    gchar *n1 = find_group(out, "N1");
    g_assert_nonnull(strstr(n3, "transform=\"translate(140.00 60.00)\""));
    g_assert_null(strstr(n1, "transform="));

    // Edges not touching N3 are byte-identical; those touching it changed.
    gchar *orig_unrel = find_group(fx.svg, "N5&#45;&gt;N6");
    gchar *new_unrel = find_group(out, "N5&#45;&gt;N6");
    g_assert_cmpstr(orig_unrel, ==, new_unrel);

    const gchar *touching[] = {"N1&#45;&gt;N3", "N2&#45;&gt;N3", "N3&#45;&gt;N4",
                               "N4&#45;&gt;N3"};
    for (guint i = 0; i < G_N_ELEMENTS(touching); i++) {
        gchar *a = find_group(fx.svg, touching[i]);
        gchar *c = find_group(out, touching[i]);
        g_assert_cmpstr(a, !=, c);
        // Still a complete edge: path + arrowhead + label.
        g_assert_nonnull(strstr(c, "<path "));
        g_assert_nonnull(strstr(c, "<polygon "));
        g_assert_nonnull(strstr(c, "<text "));
        g_free(a);
        g_free(c);
    }

    // The N1->N3 edge ends (arrow tip, the polygon's 2nd point) on N3's
    // NEW box boundary.
    gchar *edge = find_group(out, "N1&#45;&gt;N3");
    const gchar *pts = strstr(edge, "<polygon ");
    pts = strstr(pts, "points=\"") + 8;
    gchar *end1;
    g_ascii_strtod(pts, &end1);          // ax
    g_ascii_strtod(end1 + 1, &end1);     // ay
    gdouble tipx = g_ascii_strtod(end1 + 1, &end1);
    gdouble tipy = g_ascii_strtod(end1 + 1, NULL);
    const SchematicSvgNode *node3 = g_hash_table_lookup(fx.nodes, "N3");
    gdouble l = node3->cx + 140.0 - node3->w / 2 - 0.5, r = node3->cx + 140.0 + node3->w / 2 + 0.5;
    gdouble t = node3->cy + 60.0 - node3->h / 2 - 0.5, bt = node3->cy + 60.0 + node3->h / 2 + 0.5;
    g_assert_true(tipx >= l && tipx <= r && tipy >= t && tipy <= bt);
    // ...and it's on the boundary, not deep inside the box.
    gboolean on_edge = fabs(tipx - (node3->cx + 140.0 - node3->w / 2)) < 0.6 ||
                       fabs(tipx - (node3->cx + 140.0 + node3->w / 2)) < 0.6 ||
                       fabs(tipy - (node3->cy + 60.0 - node3->h / 2)) < 0.6 ||
                       fabs(tipy - (node3->cy + 60.0 + node3->h / 2)) < 0.6;
    g_assert_true(on_edge);
    g_free(edge);

    // N3<->N4 are a feedback pair: redrawn, they must not be the same
    // curve (they'd overlap exactly), i.e. their midpoints are well apart.
    {
        gchar *fwd = find_group(out, "N3&#45;&gt;N4");
        gchar *back = find_group(out, "N4&#45;&gt;N3");
        gdouble fx_, fy_, bx_, by_;
        g_assert_true(path_midpoint(fwd, &fx_, &fy_));
        g_assert_true(path_midpoint(back, &bx_, &by_));
        g_assert_cmpfloat(hypot(fx_ - bx_, fy_ - by_), >, 8.0);
        g_free(fwd);
        g_free(back);
    }

    // Bounds grew to hold the moved node, and are at least the original.
    g_assert_cmpfloat(b.width, >, fx.w);
    g_assert_cmpfloat(b.height, >, fx.h);
    gdouble nx, ny, nw, nh;
    schematic_svg_node_pixel_box(node3, g_hash_table_lookup(offsets, "N3"), &b, 96.0 / 72.0,
                                 &nx, &ny, &nw, &nh);
    g_assert_cmpfloat(nx, >=, 0.0);
    g_assert_cmpfloat(ny, >=, 0.0);
    g_assert_cmpfloat(nx + nw, <=, b.width * 96.0 / 72.0 + 0.01);
    g_assert_cmpfloat(ny + nh, <=, b.height * 96.0 / 72.0 + 0.01);

    g_free(n1); g_free(n3); g_free(orig_unrel); g_free(new_unrel);
    g_free(out);
    g_hash_table_unref(offsets);
    fixture_free(&fx);
}

static void test_svg_move_negative_grows_bounds(void)
{
    Fixture fx = {0};
    if (!fixture_load(&fx)) {
        g_test_skip("graphviz 'dot' not available");
        return;
    }
    // Pull a node up and to the left, outside the original drawing.
    GHashTable *offsets = offsets_with("N1", -200.0, -150.0);
    SchematicSvgBounds b;
    gchar *out = schematic_svg_apply_offsets(fx.svg, fx.nodes, offsets, fx.w, fx.h,
                                             96.0 / 72.0, &b, NULL);
    g_assert_nonnull(out);
    g_assert_true(well_formed(out));
    g_assert_cmpfloat(b.min_x, <, -150.0);
    g_assert_cmpfloat(b.min_y, <, -fx.h - 100.0);

    // Every node's pixel box stays inside the image (nothing clipped).
    GHashTableIter it;
    gpointer k, v;
    g_hash_table_iter_init(&it, fx.nodes);
    while (g_hash_table_iter_next(&it, &k, &v)) {
        gdouble x, y, w, h;
        schematic_svg_node_pixel_box(v, g_hash_table_lookup(offsets, k), &b, 96.0 / 72.0,
                                     &x, &y, &w, &h);
        g_assert_cmpfloat(x, >=, 0.0);
        g_assert_cmpfloat(y, >=, 0.0);
        g_assert_cmpfloat(x + w, <=, b.width * 96.0 / 72.0 + 0.01);
        g_assert_cmpfloat(y + h, <=, b.height * 96.0 / 72.0 + 0.01);
    }
    g_free(out);
    g_hash_table_unref(offsets);
    fixture_free(&fx);
}

static void test_svg_no_offsets_is_geometry_neutral(void)
{
    Fixture fx = {0};
    if (!fixture_load(&fx)) {
        g_test_skip("graphviz 'dot' not available");
        return;
    }
    SchematicSvgBounds b;
    gchar *out = schematic_svg_apply_offsets(fx.svg, fx.nodes, NULL, fx.w, fx.h, 96.0 / 72.0,
                                             &b, NULL);
    g_assert_nonnull(out);
    g_assert_true(well_formed(out));
    // No node or edge text changed; only the margin-padded root framing.
    g_assert_cmpfloat(fabs(b.width - fx.w - 4.0), <, 0.01);
    g_assert_cmpfloat(fabs(b.height - fx.h - 4.0), <, 0.01);
    gchar *a = find_group(fx.svg, "N3");
    gchar *c = find_group(out, "N3");
    g_assert_cmpstr(a, ==, c);
    g_free(a);
    g_free(c);
    g_free(out);
    fixture_free(&fx);
}

static void test_svg_rejects_non_graphviz(void)
{
    GHashTable *nodes = g_hash_table_new(g_str_hash, g_str_equal);
    GError *err = NULL;
    SchematicSvgBounds b;
    gchar *out = schematic_svg_apply_offsets("<html>nope</html>", nodes, NULL, 10, 10, 1.0, &b,
                                             &err);
    g_assert_null(out);
    g_assert_nonnull(err);
    g_clear_error(&err);
    g_hash_table_unref(nodes);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    g_test_add_func("/schematic-layout/store-basics", test_store_basics);
    g_test_add_func("/schematic-layout/store-roundtrip", test_store_roundtrip);
    g_test_add_func("/schematic-layout/store-malformed", test_store_rejects_malformed);
    g_test_add_func("/schematic-svg/move-node", test_svg_move_node);
    g_test_add_func("/schematic-svg/move-outside-grows-bounds",
                    test_svg_move_negative_grows_bounds);
    g_test_add_func("/schematic-svg/no-offsets-neutral", test_svg_no_offsets_is_geometry_neutral);
    g_test_add_func("/schematic-svg/rejects-non-graphviz", test_svg_rejects_non_graphviz);
    return g_test_run();
}

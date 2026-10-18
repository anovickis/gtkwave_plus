#include <glib.h>
#include <string.h>

#include "schematic_node_key.h"

static gchar *key_of(GHashTable *keys, const gchar *id)
{
    const gchar *k = g_hash_table_lookup(keys, id);
    g_assert_nonnull(k);
    return g_strdup(k);
}

// The same cone, numbered two different ways (as two slang-netlist runs
// would): every node must get the same key under both numberings.
static const gchar *CONE_A =
    "digraph {\n"
    "  node [shape=record];\n"
    "  N1 [label=\"In port clk\"]\n"
    "  N2 [label=\"In port rst_n\"]\n"
    "  N3 [label=\"In port en\"]\n"
    "  N19 [label=\"Conditional\"]\n"
    "  N22 [label=\"Conditional\"]\n"
    "  N23 [label=\"Assignment\"]\n"
    "  N26 [label=\"count_q [3:0]\"]\n"
    "  N1 -> N19 [label=\"clk[0]\"]\n"
    "  N2 -> N19 [label=\"rst_n[0]\"]\n"
    "  N3 -> N22 [label=\"en[0]\"]\n"
    "  N19 -> N23\n"
    "  N22 -> N23\n"
    "  N23 -> N26 [label=\"count_q[3:0]\"]\n"
    "  N26 -> N23 [label=\"count_q[3:0]\"]\n"
    "}\n";

static const gchar *CONE_B = // same graph, ids permuted, lines reordered
    "digraph {\n"
    "  node [shape=record];\n"
    "  N30 [label=\"count_q [3:0]\"]\n"
    "  N28 [label=\"Assignment\"]\n"
    "  N24 [label=\"Conditional\"]\n"
    "  N21 [label=\"Conditional\"]\n"
    "  N9 [label=\"In port en\"]\n"
    "  N8 [label=\"In port rst_n\"]\n"
    "  N7 [label=\"In port clk\"]\n"
    "  N28 -> N30 [label=\"count_q[3:0]\"]\n"
    "  N24 -> N28\n"
    "  N30 -> N28 [label=\"count_q[3:0]\"]\n"
    "  N21 -> N28\n"
    "  N9 -> N24 [label=\"en[0]\"]\n"
    "  N8 -> N21 [label=\"rst_n[0]\"]\n"
    "  N7 -> N21 [label=\"clk[0]\"]\n"
    "}\n";

static void test_stable_across_renumbering(void)
{
    GHashTable *a = schematic_node_keys(CONE_A);
    GHashTable *b = schematic_node_keys(CONE_B);
    g_assert_cmpuint(g_hash_table_size(a), ==, 7);
    g_assert_cmpuint(g_hash_table_size(b), ==, 7);

    // (A id, B id) pairs naming the same node.
    const gchar *same[][2] = {{"N1", "N7"},   {"N2", "N8"},   {"N3", "N9"},  {"N19", "N21"},
                              {"N22", "N24"}, {"N23", "N28"}, {"N26", "N30"}};
    for (guint i = 0; i < G_N_ELEMENTS(same); i++) {
        gchar *ka = key_of(a, same[i][0]);
        gchar *kb = key_of(b, same[i][1]);
        g_assert_cmpstr(ka, ==, kb);
        g_free(ka);
        g_free(kb);
    }

    // And within one graph, the two identically-labelled "Conditional"
    // nodes (different inputs) are told apart.
    gchar *c1 = key_of(a, "N19");
    gchar *c2 = key_of(a, "N22");
    g_assert_cmpstr(c1, !=, c2);
    g_free(c1);
    g_free(c2);

    GHashTable *distinct = g_hash_table_new(g_str_hash, g_str_equal);
    GHashTableIter it;
    gpointer k, v;
    g_hash_table_iter_init(&it, a);
    while (g_hash_table_iter_next(&it, &k, &v)) {
        g_hash_table_add(distinct, v);
    }
    g_assert_cmpuint(g_hash_table_size(distinct), ==, 7);

    g_hash_table_unref(distinct);
    g_hash_table_unref(a);
    g_hash_table_unref(b);
}

static void test_changed_structure_changes_key(void)
{
    // Same labels but "Assignment" now also fed by clk: its key (and its
    // neighbours') must change, so a stale override doesn't follow it.
    gchar *edited = g_strdup_printf("%s", CONE_A);
    gchar *pos = strstr(edited, "}\n");
    *pos = '\0';
    gchar *with_extra = g_strconcat(edited, "  N1 -> N23 [label=\"x\"]\n}\n", NULL);

    GHashTable *a = schematic_node_keys(CONE_A);
    GHashTable *c = schematic_node_keys(with_extra);
    gchar *ka = key_of(a, "N23");
    gchar *kc = key_of(c, "N23");
    g_assert_cmpstr(ka, !=, kc);

    g_free(ka);
    g_free(kc);
    g_free(edited);
    g_free(with_extra);
    g_hash_table_unref(a);
    g_hash_table_unref(c);
}

static void test_twins_share_a_key(void)
{
    // Two structurally identical branches are indistinguishable; they get
    // the same key (callers must break the tie).
    GHashTable *k = schematic_node_keys(
        "digraph {\n"
        "  N1 [label=\"In port a\"]\n"
        "  N2 [label=\"Assignment\"]\n"
        "  N3 [label=\"Assignment\"]\n"
        "  N4 [label=\"out\"]\n"
        "  N1 -> N2\n  N1 -> N3\n  N2 -> N4\n  N3 -> N4\n"
        "}\n");
    gchar *k2 = key_of(k, "N2");
    gchar *k3 = key_of(k, "N3");
    g_assert_cmpstr(k2, ==, k3);
    g_free(k2);
    g_free(k3);
    g_hash_table_unref(k);
}

static void test_degenerate_input(void)
{
    GHashTable *k = schematic_node_keys(NULL);
    g_assert_cmpuint(g_hash_table_size(k), ==, 0);
    g_hash_table_unref(k);

    k = schematic_node_keys("not a dot file at all");
    g_assert_cmpuint(g_hash_table_size(k), ==, 0);
    g_hash_table_unref(k);

    // Edge to an undeclared node is ignored, not a crash.
    k = schematic_node_keys("digraph {\n  N1 [label=\"a\"]\n  N1 -> N99\n}\n");
    g_assert_cmpuint(g_hash_table_size(k), ==, 1);
    g_hash_table_unref(k);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    g_test_add_func("/schematic-node-key/stable-across-renumbering",
                    test_stable_across_renumbering);
    g_test_add_func("/schematic-node-key/structure-change", test_changed_structure_changes_key);
    g_test_add_func("/schematic-node-key/twins", test_twins_share_a_key);
    g_test_add_func("/schematic-node-key/degenerate", test_degenerate_input);
    return g_test_run();
}

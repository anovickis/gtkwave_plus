#include <gtkwave.h>
#include <string.h>

/*
 * These tests exercise the real slang-netlist subprocess end-to-end
 * (lib/libgtkwave/test/files/netlist_spike_design.sv), rather than mocking
 * it, since the entire point of this module is the subprocess/JSON
 * contract with an external, independently-evolving tool. They skip
 * cleanly when slang-netlist isn't installed (it's an optional, separately
 * built dependency -- see gw-netlist-query.h).
 */

static gboolean skip_if_no_binary(void)
{
    gchar *binary = gw_netlist_query_find_binary();
    if (binary == NULL) {
        g_test_skip("slang-netlist not found on $PATH");
        return TRUE;
    }
    g_free(binary);
    return FALSE;
}

static GwNetlistQuery *new_query_for_test_design(void)
{
    gchar *design = g_build_filename("files", "netlist_spike_design.sv", NULL);
    const gchar *sources[] = {design};
    GwNetlistQuery *query = gw_netlist_query_new(sources, 1, "top");
    g_free(design);
    return query;
}

static void test_get_drivers(void)
{
    if (skip_if_no_binary()) {
        return;
    }

    GwNetlistQuery *query = new_query_for_test_design();

    GError *error = NULL;
    GPtrArray *edges = gw_netlist_query_get_drivers(query, "top.u_counter.count_q", &error);
    g_assert_no_error(error);
    g_assert_nonnull(edges);
    g_assert_cmpuint(edges->len, ==, 2);

    gboolean saw_assignment = FALSE;
    gboolean saw_register = FALSE;
    for (guint i = 0; i < edges->len; i++) {
        GwNetlistEdge *edge = g_ptr_array_index(edges, i);
        g_assert_cmpstr(edge->bits, ==, "[3:0]");
        g_assert_nonnull(edge->description);
        g_assert_nonnull(edge->location);

        if (g_str_equal(edge->description, "assignment")) {
            saw_assignment = TRUE;
        } else if (g_str_has_prefix(edge->description, "register")) {
            saw_register = TRUE;
            g_assert_true(g_str_has_suffix(edge->description, "top.u_counter.count_q"));
        }
    }
    g_assert_true(saw_assignment);
    g_assert_true(saw_register);

    g_ptr_array_unref(edges);
    g_object_unref(query);
}

static void test_get_loads(void)
{
    if (skip_if_no_binary()) {
        return;
    }

    GwNetlistQuery *query = new_query_for_test_design();

    GError *error = NULL;
    GPtrArray *edges = gw_netlist_query_get_loads(query, "top.u_counter.count_q", &error);
    g_assert_no_error(error);
    g_assert_nonnull(edges);
    // The queried signal itself, plus at least: the `count` port assignment
    // and the `value` top-level assignment it fans out to.
    g_assert_cmpuint(edges->len, >=, 3);

    gboolean saw_count = FALSE;
    gboolean saw_value = FALSE;
    for (guint i = 0; i < edges->len; i++) {
        GwNetlistEdge *edge = g_ptr_array_index(edges, i);
        g_assert_nonnull(edge->path);
        g_assert_nonnull(edge->location);

        if (g_str_equal(edge->path, "top.u_counter.count")) {
            saw_count = TRUE;
        } else if (g_str_equal(edge->path, "top.value")) {
            saw_value = TRUE;
        }
    }
    g_assert_true(saw_count);
    g_assert_true(saw_value);

    g_ptr_array_unref(edges);
    g_object_unref(query);
}

static void test_get_fanin(void)
{
    if (skip_if_no_binary()) {
        return;
    }

    GwNetlistQuery *query = new_query_for_test_design();

    GError *error = NULL;
    GPtrArray *edges = gw_netlist_query_get_fanin(query, "top.u_counter.count_q", &error);
    g_assert_no_error(error);
    g_assert_nonnull(edges);
    // The register's own clock/reset/enable control signals should all show
    // up in its fan-in cone.
    g_assert_cmpuint(edges->len, >=, 3);

    gboolean saw_clk = FALSE;
    gboolean saw_rst_n = FALSE;
    gboolean saw_en = FALSE;
    for (guint i = 0; i < edges->len; i++) {
        GwNetlistEdge *edge = g_ptr_array_index(edges, i);
        g_assert_nonnull(edge->path);
        g_assert_nonnull(edge->location);

        if (g_str_equal(edge->path, "top.u_counter.clk")) {
            saw_clk = TRUE;
        } else if (g_str_equal(edge->path, "top.u_counter.rst_n")) {
            saw_rst_n = TRUE;
        } else if (g_str_equal(edge->path, "top.u_counter.en")) {
            saw_en = TRUE;
        }
    }
    g_assert_true(saw_clk);
    g_assert_true(saw_rst_n);
    g_assert_true(saw_en);

    g_ptr_array_unref(edges);
    g_object_unref(query);
}

static void test_get_cone_dot(void)
{
    if (skip_if_no_binary()) {
        return;
    }

    GwNetlistQuery *query = new_query_for_test_design();

    GError *error = NULL;
    gchar *dot = gw_netlist_query_get_cone_dot(query, "top.u_counter.count_q", &error);
    g_assert_no_error(error);
    g_assert_nonnull(dot);
    g_assert_true(g_str_has_prefix(dot, "digraph"));
    // Node/edge lines should be present for the register's own control
    // signals, same as the flat fan-in query.
    g_assert_nonnull(strstr(dot, "clk"));
    g_assert_nonnull(strstr(dot, "rst_n"));
    g_assert_nonnull(strstr(dot, "->"));

    g_free(dot);
    g_object_unref(query);
}

static void test_spawn_failure_reports_error(void)
{
    // Doesn't need slang-netlist installed -- this exercises the failure
    // path when the configured binary doesn't exist at all.
    const gchar *sources[] = {"unused.sv"};
    GwNetlistQuery *query = gw_netlist_query_new(sources, 1, "top");
    gw_netlist_query_set_binary_path(query, "/nonexistent/path/to/slang-netlist");

    GError *error = NULL;
    GPtrArray *edges = gw_netlist_query_get_drivers(query, "top.sig", &error);
    g_assert_null(edges);
    g_assert_error(error, GW_NETLIST_QUERY_ERROR, GW_NETLIST_QUERY_ERROR_SPAWN_FAILED);
    g_clear_error(&error);

    g_object_unref(query);
}

int main(int argc, char *argv[])
{
    g_test_init(&argc, &argv, NULL);
    g_test_add_func("/netlist_query/get_drivers", test_get_drivers);
    g_test_add_func("/netlist_query/get_loads", test_get_loads);
    g_test_add_func("/netlist_query/get_fanin", test_get_fanin);
    g_test_add_func("/netlist_query/get_cone_dot", test_get_cone_dot);
    g_test_add_func("/netlist_query/spawn_failure_reports_error",
                    test_spawn_failure_reports_error);
    return g_test_run();
}

#include "netlist_graph.h"

#include <string.h>

static NetlistGraph *graph;
static gboolean listed(GPtrArray *a, const gchar *s);

static const NetlistSource *find_source(GPtrArray *sources, const gchar *signal, NetlistEdgeKind kind)
{
    for (guint i = 0; i < sources->len; i++) {
        const NetlistSource *s = g_ptr_array_index(sources, i);
        if (s->kind == kind && g_str_equal(s->signal, signal)) {
            return s;
        }
    }
    return NULL;
}

static void test_basic(void)
{
    g_assert_cmpstr(netlist_graph_top(graph), ==, "cpu_top");
    g_assert_true(netlist_graph_has_signal(graph, "cpu_top.acc"));
    g_assert_false(netlist_graph_has_signal(graph, "cpu_top.nope"));
    g_assert_true(netlist_graph_is_register(graph, "cpu_top.acc"));
    g_assert_false(netlist_graph_is_register(graph, "cpu_top.result"));

    NetlistLoc loc = {0};
    g_assert_true(netlist_graph_declaration(graph, "cpu_top.acc", &loc));
    g_assert_cmpuint(loc.line, ==, 36);
    g_assert_nonnull(loc.file);
    g_free(loc.file);
    g_assert_false(netlist_graph_declaration(graph, "cpu_top.nope", &loc));
}

// result = acc ^ din: both are sources, through the assignment on line 40.
// (slang-netlist's own --fan-in drops acc, a register; the graph keeps it.)
static void test_sources_through_register(void)
{
    GPtrArray *src = netlist_graph_sources(graph, "cpu_top.result");
    g_assert_nonnull(src);
    g_assert_cmpuint(src->len, ==, 2);
    const NetlistSource *acc = find_source(src, "cpu_top.acc", NETLIST_EDGE_DATA);
    g_assert_nonnull(acc);
    g_assert_cmpuint(acc->statements->len, ==, 1);
    g_assert_cmpuint(((NetlistLoc *)g_ptr_array_index(acc->statements, 0))->line, ==, 40);
    g_assert_nonnull(find_source(src, "cpu_top.din", NETLIST_EDGE_DATA));
    g_ptr_array_unref(src);
}

static void test_register_clock(void)
{
    GPtrArray *src = netlist_graph_sources(graph, "cpu_top.acc");
    g_assert_nonnull(src);
    g_assert_nonnull(find_source(src, "cpu_top.clk", NETLIST_EDGE_POSEDGE));
    g_assert_nonnull(find_source(src, "cpu_top.u_alu.y", NETLIST_EDGE_DATA));
    // Data comes from the ALU output only; the clock isn't also a data source.
    g_assert_null(find_source(src, "cpu_top.clk", NETLIST_EDGE_DATA));
    g_ptr_array_unref(src);
}

// `wire rd` joins two instances and has no node in the netlist; it is
// recovered from the edges that carry it.
static void test_wires(void)
{
    g_assert_true(netlist_graph_has_signal(graph, "cpu_top.rd"));
    GPtrArray *src = netlist_graph_sources(graph, "cpu_top.rd");
    g_assert_cmpuint(src->len, ==, 1);
    g_assert_nonnull(find_source(src, "cpu_top.u_rf.rd", NETLIST_EDGE_DATA));
    g_ptr_array_unref(src);

    src = netlist_graph_sources(graph, "cpu_top.alu_y");
    g_assert_nonnull(find_source(src, "cpu_top.u_alu.y", NETLIST_EDGE_DATA));
    g_ptr_array_unref(src);

    gchar *prefix = NULL;
    gchar *n = netlist_graph_resolve_dump_path(graph, "tb.dut.rd", &prefix);
    g_assert_cmpstr(n, ==, "cpu_top.rd");
    g_free(n);
    g_free(prefix);

    // A wire is not a register and has no declaration line of its own.
    g_assert_false(netlist_graph_is_register(graph, "cpu_top.rd"));
    GPtrArray *at = netlist_graph_signals_at_line(graph, "gate_cone_design.sv", 30);
    g_assert_false(listed(at, "cpu_top.rd"));
    g_ptr_array_unref(at);
}

static void test_primary_input_has_no_sources(void)
{
    GPtrArray *src = netlist_graph_sources(graph, "cpu_top.din");
    g_assert_nonnull(src);
    g_assert_cmpuint(src->len, ==, 0);
    g_ptr_array_unref(src);
    g_assert_null(netlist_graph_sources(graph, "cpu_top.nope"));
}

static void test_driver_statements(void)
{
    GPtrArray *d = netlist_graph_driver_statements(graph, "cpu_top.result");
    g_assert_cmpuint(d->len, ==, 1);
    g_assert_cmpuint(((NetlistLoc *)g_ptr_array_index(d, 0))->line, ==, 40);
    g_ptr_array_unref(d);

    d = netlist_graph_driver_statements(graph, "cpu_top.acc");
    g_assert_cmpuint(d->len, ==, 1);
    g_assert_cmpuint(((NetlistLoc *)g_ptr_array_index(d, 0))->line, ==, 39);
    g_ptr_array_unref(d);

    d = netlist_graph_driver_statements(graph, "cpu_top.din");
    g_assert_cmpuint(d->len, ==, 0);
    g_ptr_array_unref(d);
}

static gboolean listed(GPtrArray *a, const gchar *s)
{
    for (guint i = 0; i < a->len; i++) {
        if (g_str_equal(g_ptr_array_index(a, i), s)) {
            return TRUE;
        }
    }
    return FALSE;
}

static void test_signals_at_line(void)
{
    // line 36 declares acc (and its port list neighbours do not).
    GPtrArray *a = netlist_graph_signals_at_line(graph, "gate_cone_design.sv", 36);
    g_assert_true(listed(a, "cpu_top.acc"));
    g_ptr_array_unref(a);

    // line 40 is `assign result = acc ^ din;`
    a = netlist_graph_signals_at_line(graph, "lib/libgtkwave/test/files/gate_cone_design.sv", 40);
    g_assert_true(listed(a, "cpu_top.result"));
    g_assert_false(listed(a, "cpu_top.acc"));
    g_ptr_array_unref(a);

    a = netlist_graph_signals_at_line(graph, "gate_cone_design.sv", 9999);
    g_assert_cmpuint(a->len, ==, 0);
    g_ptr_array_unref(a);
    a = netlist_graph_signals_at_line(graph, "other.sv", 36);
    g_assert_cmpuint(a->len, ==, 0);
    g_ptr_array_unref(a);
}

static void test_name_mapping(void)
{
    gchar *prefix = NULL;
    gchar *n = netlist_graph_resolve_dump_path(graph, "tb.dut.u_rf.r0", &prefix);
    g_assert_cmpstr(n, ==, "cpu_top.u_rf.r0");
    g_assert_cmpstr(prefix, ==, "tb.dut");
    gchar *back = netlist_graph_to_dump_path(graph, prefix, "cpu_top.acc");
    g_assert_cmpstr(back, ==, "tb.dut.acc");
    g_free(back);
    g_free(n);
    g_free(prefix);

    // Dump rooted at the top module itself, with a bit range on the name.
    n = netlist_graph_resolve_dump_path(graph, "cpu_top.acc[3:0]", &prefix);
    g_assert_cmpstr(n, ==, "cpu_top.acc");
    g_assert_cmpstr(prefix, ==, "cpu_top");
    g_free(n);
    g_free(prefix);

    n = netlist_graph_resolve_dump_path(graph, "tb.dut.not_there", &prefix);
    g_assert_null(n);
    g_assert_null(netlist_graph_to_dump_path(graph, "tb", "other_top.x"));
}

static void test_bad_input(void)
{
    GError *error = NULL;
    g_assert_null(netlist_graph_new("not json", &error));
    g_assert_nonnull(error);
    g_clear_error(&error);
    g_assert_null(netlist_graph_new("[]", &error));
    g_clear_error(&error);
    g_assert_null(netlist_graph_new("{\"nodes\":[],\"edges\":[]}", &error));
    g_assert_error(error, NETLIST_GRAPH_ERROR, NETLIST_GRAPH_ERROR_PARSE);
    g_clear_error(&error);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    g_assert_cmpint(argc, ==, 2);

    gchar *text = NULL;
    GError *error = NULL;
    if (!g_file_get_contents(argv[1], &text, NULL, &error)) {
        g_error("%s", error->message);
    }
    graph = netlist_graph_new(text, &error);
    if (graph == NULL) {
        g_error("%s", error->message);
    }
    g_free(text);

    g_test_add_func("/netlist_graph/basic", test_basic);
    g_test_add_func("/netlist_graph/sources_through_register", test_sources_through_register);
    g_test_add_func("/netlist_graph/register_clock", test_register_clock);
    g_test_add_func("/netlist_graph/wires", test_wires);
    g_test_add_func("/netlist_graph/primary_input", test_primary_input_has_no_sources);
    g_test_add_func("/netlist_graph/driver_statements", test_driver_statements);
    g_test_add_func("/netlist_graph/signals_at_line", test_signals_at_line);
    g_test_add_func("/netlist_graph/name_mapping", test_name_mapping);
    g_test_add_func("/netlist_graph/bad_input", test_bad_input);

    gint rc = g_test_run();
    netlist_graph_free(graph);
    return rc;
}

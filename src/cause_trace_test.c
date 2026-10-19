#include "cause_trace.h"

typedef struct
{
    const gchar *path;
    const gchar *spec; // "time:value ..." pairs
} Canned;

static const Canned *canned = NULL;
static NetlistGraph *graph;

static CauseHistory *provide(const gchar *dump_path, gpointer user_data)
{
    (void)user_data;
    for (const Canned *c = canned; c->path != NULL; c++) {
        if (!g_str_equal(c->path, dump_path)) {
            continue;
        }
        CauseHistory *h = cause_history_new();
        gchar **pairs = g_strsplit(c->spec, " ", -1);
        for (gchar **p = pairs; *p != NULL; p++) {
            gchar **kv = g_strsplit(*p, ":", 2);
            cause_history_append(h, g_ascii_strtoll(kv[0], NULL, 10), kv[1]);
            g_strfreev(kv);
        }
        g_strfreev(pairs);
        return h;
    }
    return NULL;
}

static const Canned base[] = {
    {"tb.dut.clk", "0:0 5:1 10:0 15:1 20:0 25:1 30:0"},
    {"tb.dut.din", "0:5"},
    {"tb.dut.u_alu.y", "0:0 12:3 25:4"},
    {"tb.dut.acc", "0:0 15:3 25:9"},
    {"tb.dut.result", "0:0 15:6 40:1"},
    {NULL, NULL},
};

static CauseContext *make(const Canned *data)
{
    canned = data;
    return cause_context_new(graph, "tb.dut", provide, NULL);
}

static const CauseItem *find(const CauseExplanation *e, const gchar *signal)
{
    for (guint i = 0; i < e->items->len; i++) {
        const CauseItem *it = g_ptr_array_index(e->items, i);
        if (g_str_equal(it->signal, signal)) {
            return it;
        }
    }
    return NULL;
}

static void test_history_folding(void)
{
    CauseHistory *h = cause_history_new();
    cause_history_append(h, 0, "0");
    cause_history_append(h, 5, "0"); // no change: folded
    cause_history_append(h, 7, "1");
    cause_history_append(h, 7, "0"); // glitch back at the same time: change vanishes
    g_assert_cmpuint(h->n, ==, 1);
    cause_history_free(h);
}

static void test_register_sampled(void)
{
    CauseContext *ctx = make(base);
    // acc went 0 -> 3 at the edge at 15; y moved to 3 at 12, between edges 5 and 15.
    CauseExplanation *e = cause_explain(ctx, "cpu_top.acc", 16);
    g_assert_true(e->found_change);
    g_assert_cmpint(e->change_time, ==, 15);
    g_assert_cmpstr(e->old_value, ==, "0");
    g_assert_cmpstr(e->new_value, ==, "3");
    g_assert_true(e->is_register);
    g_assert_null(e->note);
    g_assert_cmpstr(e->dump_path, ==, "tb.dut.acc");

    const CauseItem *clk = find(e, "cpu_top.clk");
    g_assert_nonnull(clk);
    g_assert_cmpint(clk->kind, ==, CAUSE_TRIGGER);
    g_assert_cmpint(clk->time, ==, 15);
    const CauseItem *y = find(e, "cpu_top.u_alu.y");
    g_assert_cmpint(y->kind, ==, CAUSE_SAMPLED);
    g_assert_true(y->likely);
    g_assert_cmpint(y->time, ==, 12);
    g_assert_cmpstr(y->new_value, ==, "3");
    g_assert_cmpuint(y->statements->len, >=, 1);
    // Trigger sorts first, then likely causes.
    g_assert_cmpint(((CauseItem *)g_ptr_array_index(e->items, 0))->kind, ==, CAUSE_TRIGGER);
    cause_explanation_free(e);
    cause_context_free(ctx);
}

static void test_same_time_is_not_seen_by_the_flop(void)
{
    CauseContext *ctx = make(base);
    // acc changed again at the edge at 25, but y also changed AT 25 (a flop
    // does not see that) and had not moved since 12 -- before the edge at 15.
    CauseExplanation *e = cause_explain(ctx, "cpu_top.acc", 25);
    g_assert_cmpint(e->change_time, ==, 25);
    const CauseItem *y = find(e, "cpu_top.u_alu.y");
    g_assert_cmpint(y->kind, ==, CAUSE_SAME_TIME);
    g_assert_false(y->likely);
    g_assert_cmpint(find(e, "cpu_top.clk")->kind, ==, CAUSE_TRIGGER);
    cause_explanation_free(e);
    cause_context_free(ctx);
}

static void test_combinational_changed(void)
{
    CauseContext *ctx = make(base);
    CauseExplanation *e = cause_explain(ctx, "cpu_top.result", 17);
    g_assert_cmpint(e->change_time, ==, 15);
    g_assert_false(e->is_register);
    const CauseItem *acc = find(e, "cpu_top.acc");
    g_assert_cmpint(acc->kind, ==, CAUSE_CHANGED);
    g_assert_true(acc->likely);
    g_assert_cmpint(acc->time, ==, 15);
    const CauseItem *din = find(e, "cpu_top.din");
    g_assert_cmpint(din->kind, ==, CAUSE_HELD);
    g_assert_false(din->likely);
    g_assert_cmpstr(din->new_value, ==, "5");
    g_assert_cmpstr(((CauseItem *)g_ptr_array_index(e->items, 0))->signal, ==, "cpu_top.acc");
    cause_explanation_free(e);
    cause_context_free(ctx);
}

static void test_combinational_earlier(void)
{
    CauseContext *ctx = make(base);
    // result moved at 40, when no input did: acc (moved at 25) is offered.
    CauseExplanation *e = cause_explain(ctx, "cpu_top.result", 40);
    g_assert_cmpint(e->change_time, ==, 40);
    const CauseItem *acc = find(e, "cpu_top.acc");
    g_assert_cmpint(acc->kind, ==, CAUSE_EARLIER);
    g_assert_true(acc->likely);
    g_assert_cmpint(acc->time, ==, 25);
    g_assert_nonnull(e->note);
    cause_explanation_free(e);
    cause_context_free(ctx);
}

static void test_initial_value_is_not_a_cause(void)
{
    CauseContext *ctx = make(base);
    // At time 0 result has only its initial value: say so, and don't
    // present an input's initial value as the thing that moved it.
    CauseExplanation *e = cause_explain(ctx, "cpu_top.result", 0);
    g_assert_true(e->found_change);
    g_assert_cmpint(e->change_time, ==, 0);
    g_assert_null(e->old_value);
    g_assert_nonnull(e->note);
    for (guint i = 0; i < e->items->len; i++) {
        g_assert_false(((CauseItem *)g_ptr_array_index(e->items, i))->likely);
    }
    cause_explanation_free(e);

    // A held input whose only value is its initial one is not EARLIER either.
    e = cause_explain(ctx, "cpu_top.result", 100);
    const CauseItem *din = find(e, "cpu_top.din");
    g_assert_cmpint(din->kind, ==, CAUSE_HELD);
    g_assert_false(din->moved);
    const CauseItem *acc = find(e, "cpu_top.acc");
    g_assert_true(acc->moved);
    cause_explanation_free(e);
    cause_context_free(ctx);
}

static void test_register_without_edge(void)
{
    static const Canned data[] = {
        {"tb.dut.clk", "0:0 5:1 10:0"},
        {"tb.dut.u_alu.y", "0:0"},
        {"tb.dut.acc", "0:0 7:3"}, // changed with no clock edge at 7
        {NULL, NULL},
    };
    CauseContext *ctx = make(data);
    CauseExplanation *e = cause_explain(ctx, "cpu_top.acc", 8);
    g_assert_cmpint(e->change_time, ==, 7);
    g_assert_nonnull(e->note);
    g_assert_cmpint(find(e, "cpu_top.clk")->kind, !=, CAUSE_TRIGGER);
    cause_explanation_free(e);
    cause_context_free(ctx);
}

static void test_primary_and_missing(void)
{
    CauseContext *ctx = make(base);
    CauseExplanation *e = cause_explain(ctx, "cpu_top.din", 100);
    g_assert_true(e->is_primary);
    g_assert_true(e->found_change);
    g_assert_cmpuint(e->items->len, ==, 0);
    cause_explanation_free(e);

    e = cause_explain(ctx, "cpu_top.nope", 5);
    g_assert_false(e->found_change);
    g_assert_nonnull(e->note);
    cause_explanation_free(e);

    // Before the dump has any value.
    e = cause_explain(ctx, "cpu_top.acc", -5);
    g_assert_false(e->found_change);
    cause_explanation_free(e);
    cause_context_free(ctx);

    // A signal the dump lacks.
    static const Canned sparse[] = {{"tb.dut.clk", "0:0"}, {NULL, NULL}};
    ctx = make(sparse);
    e = cause_explain(ctx, "cpu_top.acc", 5);
    g_assert_false(e->found_change);
    g_assert_nonnull(e->note);
    cause_explanation_free(e);
    cause_context_free(ctx);
}

static void test_input_missing_from_dump(void)
{
    static const Canned data[] = {
        {"tb.dut.clk", "0:0 5:1"},
        {"tb.dut.acc", "0:0 5:3"},
        {NULL, NULL},
    };
    CauseContext *ctx = make(data);
    CauseExplanation *e = cause_explain(ctx, "cpu_top.acc", 5);
    g_assert_cmpint(find(e, "cpu_top.u_alu.y")->kind, ==, CAUSE_NO_HISTORY);
    g_assert_cmpint(find(e, "cpu_top.clk")->kind, ==, CAUSE_TRIGGER);
    cause_explanation_free(e);
    cause_context_free(ctx);
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

    g_test_add_func("/cause_trace/history_folding", test_history_folding);
    g_test_add_func("/cause_trace/register_sampled", test_register_sampled);
    g_test_add_func("/cause_trace/same_time", test_same_time_is_not_seen_by_the_flop);
    g_test_add_func("/cause_trace/comb_changed", test_combinational_changed);
    g_test_add_func("/cause_trace/comb_earlier", test_combinational_earlier);
    g_test_add_func("/cause_trace/initial_value", test_initial_value_is_not_a_cause);
    g_test_add_func("/cause_trace/register_without_edge", test_register_without_edge);
    g_test_add_func("/cause_trace/primary_and_missing", test_primary_and_missing);
    g_test_add_func("/cause_trace/input_missing", test_input_missing_from_dump);

    gint rc = g_test_run();
    netlist_graph_free(graph);
    return rc;
}

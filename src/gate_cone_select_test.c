#include <stdarg.h>
#include <unistd.h>
#include <glib.h>
#include <glib/gstdio.h>
#include <string.h>

#include "gate_cone_select.h"

static const gchar *design_path = NULL;

static GPtrArray *exps(const gchar *first, ...)
{
    // (seed, hops) pairs, terminated by NULL seed.
    GPtrArray *a = g_ptr_array_new_with_free_func((GDestroyNotify)gate_expansion_free);
    va_list ap;
    va_start(ap, first);
    const gchar *seed = first;
    while (seed != NULL) {
        guint hops = va_arg(ap, guint);
        g_ptr_array_add(a, gate_expansion_new(seed, hops));
        seed = va_arg(ap, const gchar *);
    }
    va_end(ap);
    return a;
}

static GPtrArray *strs(const gchar *first, ...)
{
    GPtrArray *a = g_ptr_array_new_with_free_func(g_free);
    va_list ap;
    va_start(ap, first);
    for (const gchar *s = first; s != NULL; s = va_arg(ap, const gchar *)) {
        g_ptr_array_add(a, g_strdup(s));
    }
    va_end(ap);
    return a;
}

/* ------------------------------------------------- command building -- */

static void test_expression_shapes(void)
{
    gchar *s = gate_cone_select_expression("top/q", 2, NULL, NULL);
    g_assert_cmpstr(s, ==, "top/q %ci2");
    g_free(s);

    s = gate_cone_select_expression("top/q", 0, NULL, NULL);
    g_assert_cmpstr(s, ==, "top/q %ci*");
    g_free(s);

    GPtrArray *e = exps("top/a", 1u, "top/b", 3u, NULL);
    s = gate_cone_select_expression("top/q", 2, e, NULL);
    g_assert_cmpstr(s, ==, "top/q %ci2 top/a %ci1 top/b %ci3 %u %u");
    g_free(s);

    // Filters: N objects need N-1 unions then one difference.
    GPtrArray *f = strs("top/x", "top/y", "top/z", NULL);
    s = gate_cone_select_expression("top/q", 2, NULL, f);
    g_assert_cmpstr(s, ==, "top/q %ci2 top/x top/y top/z %u %u %d");
    g_free(s);

    // Expansions are folded in BEFORE the filter is subtracted.
    s = gate_cone_select_expression("top/q", 2, e, f);
    g_assert_cmpstr(s, ==, "top/q %ci2 top/a %ci1 top/b %ci3 %u %u top/x top/y top/z %u %u %d");
    g_free(s);

    // A zero-hop expansion must not become %ci* (the whole upstream design).
    GPtrArray *zero = exps("top/a", 0u, NULL);
    s = gate_cone_select_expression("top/q", 2, zero, NULL);
    g_assert_cmpstr(s, ==, "top/q %ci2 top/a %ci1 %u");
    g_free(s);

    g_ptr_array_unref(e);
    g_ptr_array_unref(f);
    g_ptr_array_unref(zero);
}

static void test_probe_script_and_parse(void)
{
    GPtrArray *e = exps("top/a", 1u, "top/b", 1u, NULL);
    gchar *script = gate_cone_probe_script(e);
    g_assert_cmpstr(script, ==,
                    "log GTKWAVE_PROBE top/a; select -count top/a; "
                    "log GTKWAVE_PROBE top/b; select -count top/b; ");
    g_free(script);

    script = gate_cone_probe_script(NULL);
    g_assert_cmpstr(script, ==, "");
    g_free(script);

    // Log shape taken from a real Yosys run, with noise between marker and
    // count, and a probe with no result at all (must NOT count as missing).
    const gchar *log =
        "GTKWAVE_PROBE top/a\n"
        "\n"
        "23. Executing SELECT pass.\n"
        "3 objects.\n"
        "GTKWAVE_PROBE top/b\n"
        "24. Executing SELECT pass.\n"
        "0 objects.\n"
        "GTKWAVE_PROBE top/c\n";
    GPtrArray *three = exps("top/a", 1u, "top/b", 1u, "top/c", 1u, NULL);
    GPtrArray *missing = gate_cone_missing_seeds(log, three);
    g_assert_cmpuint(missing->len, ==, 1);
    g_assert_cmpstr(g_ptr_array_index(missing, 0), ==, "top/b");
    g_ptr_array_unref(missing);

    missing = gate_cone_missing_seeds(NULL, three);
    g_assert_cmpuint(missing->len, ==, 0);
    g_ptr_array_unref(missing);

    g_ptr_array_unref(three);
    g_ptr_array_unref(e);
}

static void test_unstable_names(void)
{
    g_assert_true(gate_cone_name_is_unstable("top/$auto$ff.cc:337:slice$227"));
    g_assert_false(gate_cone_name_is_unstable("top/alu_y"));
    g_assert_false(gate_cone_name_is_unstable("top/u_alu.y"));
    g_assert_false(gate_cone_name_is_unstable("top/$and$design.sv:12$34"));
    g_assert_false(gate_cone_name_is_unstable(NULL));
}

/* ---------------------------------------------- against a real Yosys -- */

static gboolean have_yosys(void)
{
    gchar *p = g_find_program_in_path("yosys");
    g_free(p);
    return p != NULL && design_path != NULL && g_file_test(design_path, G_FILE_TEST_EXISTS);
}

// Runs the same front half of the view's pipeline on the fixture, then
// `select <expression>`, and returns the sorted selected object names
// (relative to the top module), plus the full log through *out_log.
static GPtrArray *yosys_select(const gchar *flatten, const gchar *probes, const gchar *expression,
                               gchar **out_log)
{
    gchar *list_path = NULL;
    gint fd = g_file_open_tmp("gatecone-XXXXXX.txt", &list_path, NULL);
    close(fd);

    gchar *script = g_strdup_printf(
        "read_verilog -sv -defer \"%s\"; hierarchy -check -top cpu_top; proc; %s"
        "memory_collect; opt; techmap; opt; %sselect %s; tee -q -o %s select -list",
        design_path, flatten, probes, expression, list_path);
    gchar *yosys = g_find_program_in_path("yosys");
    gchar *argv[] = {yosys, (gchar *)"-p", script, NULL};
    gchar *out = NULL;
    gint status = 0;
    g_assert_true(g_spawn_sync(NULL, argv, NULL, G_SPAWN_DEFAULT, NULL, NULL, &out, NULL, &status, NULL));
    g_assert_true(g_spawn_check_wait_status(status, NULL));

    gchar *text = NULL;
    g_assert_true(g_file_get_contents(list_path, &text, NULL, NULL));
    GPtrArray *names = g_ptr_array_new_with_free_func(g_free);
    gchar **lines = g_strsplit(text, "\n", -1);
    for (guint i = 0; lines[i] != NULL; i++) {
        if (g_str_has_prefix(lines[i], "cpu_top/")) {
            g_ptr_array_add(names, g_strdup(lines[i] + strlen("cpu_top/")));
        }
    }
    g_strfreev(lines);
    g_free(text);

    if (out_log != NULL) {
        *out_log = out;
    } else {
        g_free(out);
    }
    g_unlink(list_path);
    g_free(list_path);
    g_free(script);
    g_free(yosys);
    return names;
}

static gboolean has(GPtrArray *names, const gchar *n)
{
    for (guint i = 0; i < names->len; i++) {
        if (g_str_equal(g_ptr_array_index(names, i), n)) {
            return TRUE;
        }
    }
    return FALSE;
}

static void test_real_yosys_expansion(void)
{
    if (!have_yosys()) {
        g_test_skip("yosys or fixture design not available");
        return;
    }

    // Start small: root only. The ALU is beyond two hops.
    gchar *expr = gate_cone_select_expression("cpu_top/result", 2, NULL, NULL);
    GPtrArray *base = yosys_select("", "", expr, NULL);
    g_free(expr);
    g_assert_true(has(base, "result"));
    g_assert_true(has(base, "acc"));
    g_assert_false(has(base, "u_alu"));
    g_assert_false(has(base, "alu_y"));

    // Expand ONE net: just its driver comes in, nothing else changes.
    GPtrArray *one = exps("cpu_top/alu_y", 1u, NULL);
    expr = gate_cone_select_expression("cpu_top/result", 2, one, NULL);
    GPtrArray *grown = yosys_select("", "", expr, NULL);
    g_free(expr);
    g_assert_true(has(grown, "u_alu"));
    g_assert_true(has(grown, "alu_y"));
    g_assert_true(has(grown, "acc")); // still has the original cone
    g_assert_cmpuint(grown->len, ==, base->len + 2); // exactly alu_y + u_alu
    // ...and NOT the ALU's own inputs (that would be the next hop). (`din`
    // can't tell: result = acc ^ din already puts it in the base cone.)
    g_assert_false(has(grown, "op"));
    g_assert_false(has(grown, "rd"));

    // Expanding again from the same net goes one hop further.
    GPtrArray *two = exps("cpu_top/alu_y", 2u, NULL);
    expr = gate_cone_select_expression("cpu_top/result", 2, two, NULL);
    GPtrArray *deeper = yosys_select("", "", expr, NULL);
    g_free(expr);
    g_assert_true(has(deeper, "op"));
    g_assert_true(has(deeper, "rd"));

    g_ptr_array_unref(base);
    g_ptr_array_unref(grown);
    g_ptr_array_unref(deeper);
    g_ptr_array_unref(one);
    g_ptr_array_unref(two);
}

static void test_real_yosys_filters_with_expansions(void)
{
    if (!have_yosys()) {
        g_test_skip("yosys or fixture design not available");
        return;
    }

    // Two expansions + two filters: the %u/%d bookkeeping is where a
    // miscount would silently subtract nothing (or the wrong thing).
    GPtrArray *e = exps("cpu_top/alu_y", 1u, "cpu_top/rd", 1u, NULL);
    GPtrArray *f = strs("cpu_top/u_alu", "cpu_top/acc", NULL);
    gchar *expr = gate_cone_select_expression("cpu_top/result", 2, e, f);
    GPtrArray *sel = yosys_select("", "", expr, NULL);
    g_free(expr);

    g_assert_false(has(sel, "u_alu")); // filtered
    g_assert_false(has(sel, "acc")); // filtered
    g_assert_true(has(sel, "result")); // root survives
    g_assert_true(has(sel, "alu_y")); // expansion survives
    g_assert_true(has(sel, "u_rf")); // second expansion's driver survives

    g_ptr_array_unref(sel);
    g_ptr_array_unref(e);
    g_ptr_array_unref(f);
}

static void test_real_yosys_probe(void)
{
    if (!have_yosys()) {
        g_test_skip("yosys or fixture design not available");
        return;
    }

    GPtrArray *e = exps("cpu_top/alu_y", 1u, "cpu_top/no_such_net", 1u, NULL);
    gchar *probes = gate_cone_probe_script(e);
    gchar *expr = gate_cone_select_expression("cpu_top/result", 2, e, NULL);
    gchar *log = NULL;
    GPtrArray *sel = yosys_select("", probes, expr, &log);

    GPtrArray *missing = gate_cone_missing_seeds(log, e);
    g_assert_cmpuint(missing->len, ==, 1);
    g_assert_cmpstr(g_ptr_array_index(missing, 0), ==, "cpu_top/no_such_net");

    // The probes didn't disturb the selection that follows them.
    g_assert_true(has(sel, "u_alu"));

    g_ptr_array_unref(missing);
    g_ptr_array_unref(sel);
    g_free(log);
    g_free(expr);
    g_free(probes);
    g_ptr_array_unref(e);
}

// The reason expansions are seeded from NET names: after dissolving an
// instance, anonymous gate cells renumber while declared nets keep their
// names. If a Yosys release stopped doing this, the design's premise (and
// gate_cone_name_is_unstable()) needs revisiting.
static void test_real_yosys_name_stability_premise(void)
{
    if (!have_yosys()) {
        g_test_skip("yosys or fixture design not available");
        return;
    }
    gchar *expr = gate_cone_select_expression("cpu_top/result", 2, NULL, NULL);
    GPtrArray *plain = yosys_select("", "", expr, NULL);
    GPtrArray *flat = yosys_select("flatten cpu_top/u_alu; ", "", expr, NULL);
    g_free(expr);

    // Declared nets: identical either way.
    g_assert_true(has(plain, "acc") && has(flat, "acc"));
    g_assert_true(has(plain, "result") && has(flat, "result"));

    // Auto-named flops: the set of names differs once u_alu is flattened.
    GString *a = g_string_new(NULL), *b = g_string_new(NULL);
    for (guint i = 0; i < plain->len; i++) {
        if (gate_cone_name_is_unstable(g_ptr_array_index(plain, i))) {
            g_string_append(a, g_ptr_array_index(plain, i));
        }
    }
    for (guint i = 0; i < flat->len; i++) {
        if (gate_cone_name_is_unstable(g_ptr_array_index(flat, i))) {
            g_string_append(b, g_ptr_array_index(flat, i));
        }
    }
    g_assert_cmpuint(a->len, >, 0);
    g_assert_cmpstr(a->str, !=, b->str);

    g_string_free(a, TRUE);
    g_string_free(b, TRUE);
    g_ptr_array_unref(plain);
    g_ptr_array_unref(flat);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    if (argc > 1) {
        design_path = argv[1];
    }
    g_test_add_func("/gate-cone/expression-shapes", test_expression_shapes);
    g_test_add_func("/gate-cone/probe-script-and-parse", test_probe_script_and_parse);
    g_test_add_func("/gate-cone/unstable-names", test_unstable_names);
    g_test_add_func("/gate-cone/yosys-expansion", test_real_yosys_expansion);
    g_test_add_func("/gate-cone/yosys-filters-with-expansions",
                    test_real_yosys_filters_with_expansions);
    g_test_add_func("/gate-cone/yosys-probe", test_real_yosys_probe);
    g_test_add_func("/gate-cone/yosys-name-stability-premise",
                    test_real_yosys_name_stability_premise);
    return g_test_run();
}

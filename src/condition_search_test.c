#include "condition_search.h"

static DerivedSeries *make(guint width, const gchar *spec)
{
    // "time:bits ..." with bits MSB first, e.g. "0:0 5:1 9:x"
    DerivedSeries *s = derived_series_new(width);
    gchar **items = g_strsplit(spec, " ", -1);
    for (gchar **it = items; *it != NULL; it++) {
        gchar **kv = g_strsplit(*it, ":", 2);
        guint8 bits[16];
        for (guint b = 0; b < width; b++) {
            gchar c = kv[1][b];
            bits[b] = c == '1' ? DERIVED_BIT_1 : c == 'x' ? DERIVED_BIT_X : c == 'z' ? DERIVED_BIT_Z
                                                                                       : DERIVED_BIT_0;
        }
        derived_series_append(s, g_ascii_strtoll(kv[0], NULL, 10), bits);
        g_strfreev(kv);
    }
    g_strfreev(items);
    return s;
}

static ConditionSpan span(const ConditionResult *r, guint i)
{
    return g_array_index(r->spans, ConditionSpan, i);
}

static void test_basic_intervals(void)
{
    DerivedSeries *s = make(1, "0:0 5:1 9:0 20:1 22:0");
    ConditionResult *r = condition_search_run(s, 0, 100);
    g_assert_cmpuint(r->spans->len, ==, 2);
    g_assert_cmpint(span(r, 0).start, ==, 5);
    g_assert_cmpint(span(r, 0).end, ==, 9);
    g_assert_false(span(r, 0).open_ended);
    g_assert_cmpint(span(r, 1).start, ==, 20);
    g_assert_cmpint(span(r, 1).end, ==, 22);
    condition_result_free(r);
    derived_series_free(s);
}

static void test_min_duration(void)
{
    DerivedSeries *s = make(1, "0:0 5:1 9:0 20:1 22:0");
    ConditionResult *r = condition_search_run(s, 3, 100);
    g_assert_cmpuint(r->spans->len, ==, 1);
    g_assert_cmpint(span(r, 0).start, ==, 5);
    condition_result_free(r);
    r = condition_search_run(s, 4, 100); // exactly 4 long passes, 2 does not
    g_assert_cmpuint(r->spans->len, ==, 1);
    condition_result_free(r);
    r = condition_search_run(s, 5, 100);
    g_assert_cmpuint(r->spans->len, ==, 0);
    condition_result_free(r);
    derived_series_free(s);
}

static void test_open_ended_runs_to_dump_end(void)
{
    DerivedSeries *s = make(1, "0:0 30:1");
    ConditionResult *r = condition_search_run(s, 0, 100);
    g_assert_cmpuint(r->spans->len, ==, 1);
    g_assert_cmpint(span(r, 0).end, ==, 100);
    g_assert_true(span(r, 0).open_ended);
    condition_result_free(r);
    r = condition_search_run(s, 80, 100); // 70 long: too short
    g_assert_cmpuint(r->spans->len, ==, 0);
    condition_result_free(r);
    derived_series_free(s);
}

static void test_truth_rules(void)
{
    // Wide value: any 1 is true, even beside X; all zero false; X with no 1
    // is unknown (not true) and counted.
    DerivedSeries *s = make(4, "0:0000 2:x1x0 4:xxxx 6:0000 8:zz00");
    ConditionResult *r = condition_search_run(s, 0, 20);
    g_assert_cmpuint(r->spans->len, ==, 1);
    g_assert_cmpint(span(r, 0).start, ==, 2);
    g_assert_cmpint(span(r, 0).end, ==, 4);
    g_assert_cmpuint(r->unknown_changes, ==, 2);
    condition_result_free(r);
    derived_series_free(s);
}

static void test_constant_true_and_false(void)
{
    DerivedSeries *s = make(1, "0:1");
    ConditionResult *r = condition_search_run(s, 0, 50);
    g_assert_cmpuint(r->spans->len, ==, 1);
    g_assert_cmpint(span(r, 0).start, ==, 0);
    g_assert_cmpint(span(r, 0).end, ==, 50);
    condition_result_free(r);
    derived_series_free(s);

    s = make(1, "0:0");
    r = condition_search_run(s, 0, 50);
    g_assert_cmpuint(r->spans->len, ==, 0);
    condition_result_free(r);
    derived_series_free(s);

    s = derived_series_new(1); // no changes at all
    r = condition_search_run(s, 0, 50);
    g_assert_cmpuint(r->spans->len, ==, 0);
    condition_result_free(r);
    derived_series_free(s);
}

static void test_navigation(void)
{
    DerivedSeries *s = make(1, "0:0 5:1 9:0 20:1 22:0 40:1 41:0");
    ConditionResult *r = condition_search_run(s, 0, 100);
    g_assert_cmpuint(r->spans->len, ==, 3);
    g_assert_cmpint(condition_next_span(r, 0), ==, 0);
    g_assert_cmpint(condition_next_span(r, 5), ==, 1); // strictly after
    g_assert_cmpint(condition_next_span(r, 21), ==, 2);
    g_assert_cmpint(condition_next_span(r, 40), ==, -1);
    g_assert_cmpint(condition_prev_span(r, 5), ==, -1); // strictly before
    g_assert_cmpint(condition_prev_span(r, 6), ==, 0);
    g_assert_cmpint(condition_prev_span(r, 1000), ==, 2);
    condition_result_free(r);
    derived_series_free(s);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    g_test_add_func("/condition_search/intervals", test_basic_intervals);
    g_test_add_func("/condition_search/min_duration", test_min_duration);
    g_test_add_func("/condition_search/open_ended", test_open_ended_runs_to_dump_end);
    g_test_add_func("/condition_search/truth", test_truth_rules);
    g_test_add_func("/condition_search/constants", test_constant_true_and_false);
    g_test_add_func("/condition_search/navigation", test_navigation);
    return g_test_run();
}

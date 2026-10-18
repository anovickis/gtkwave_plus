#include <glib.h>
#include <glib/gstdio.h>
#include <stdlib.h>
#include <string.h>

#include "event_sequence.h"
#include "fstapi.h"

static const gchar *VCD_TEXT =
    "$timescale 1ns $end\n"
    "$scope module tb $end\n"
    "$var wire 1 ! clk $end\n"
    "$var wire 8 \" bus [7:0] $end\n"
    "$scope module dut $end\n"
    "$var wire 1 # a $end\n"
    "$var wire 1 $ b $end\n"
    "$var wire 1 % y $end\n"
    "$var wire 1 ! clk_alias $end\n"
    "$upscope $end\n"
    "$upscope $end\n"
    "$enddefinitions $end\n"
    "$dumpvars\n0!\nb00000000 \"\n0#\n0$\n0%\n$end\n"
    "#5\n"
    "1!\n"
    "1%\n" // y dumped BEFORE its causes a and b
    "1#\n"
    "b00001111 \"\n"
    "1$\n"
    "#10\n"
    "0!\n";

static gchar *write_tmp(const gchar *dir, const gchar *name, const gchar *text)
{
    gchar *path = g_build_filename(dir, name, NULL);
    g_assert_true(g_file_set_contents(path, text, -1, NULL));
    return path;
}

static const EventSeqChange *find(EventSeqResult *r, const gchar *path)
{
    for (guint i = 0; i < r->changes->len; i++) {
        EventSeqChange *c = g_ptr_array_index(r->changes, i);
        if (strcmp(c->path, path) == 0) {
            return c;
        }
    }
    return NULL;
}

static void test_vcd_order(void)
{
    gchar *dir = g_dir_make_tmp("evseq-XXXXXX", NULL);
    gchar *path = write_tmp(dir, "t.vcd", VCD_TEXT);

    GError *error = NULL;
    EventSeqResult *r = event_sequence_scan(path, 5, &error);
    g_assert_no_error(error);
    g_assert_nonnull(r);
    g_assert_true(r->order_known);

    // clk has two names (id-code alias): ONE change, deepest name primary.
    g_assert_cmpuint(r->changes->len, ==, 5);
    const EventSeqChange *clk = find(r, "tb.dut.clk_alias");
    const EventSeqChange *y = find(r, "tb.dut.y");
    const EventSeqChange *bus = find(r, "tb.bus");
    g_assert_nonnull(clk);
    g_assert_null(find(r, "tb.clk"));
    g_assert_cmpuint(clk->aliases->len, ==, 1);
    g_assert_cmpstr(g_ptr_array_index(clk->aliases, 0), ==, "tb.clk");
    g_assert_cmpuint(clk->seq, ==, 1);
    g_assert_cmpuint(y->seq, ==, 2);
    g_assert_cmpstr(bus->value, ==, "b00001111"); // bit-range stripped from path
    g_assert_cmpstr(clk->value, ==, "1");

    // Changes at other times must not leak in.
    event_sequence_result_free(r);
    r = event_sequence_scan(path, 10, &error);
    g_assert_no_error(error);
    g_assert_cmpuint(r->changes->len, ==, 1);
    event_sequence_result_free(r);

    r = event_sequence_scan(path, 7, &error);
    g_assert_no_error(error);
    g_assert_cmpuint(r->changes->len, ==, 0);
    event_sequence_result_free(r);

    // The $dumpvars initial block belongs to time 0 (no explicit #0 line:
    // the dump starts before any timestamp, so it is not a change at 0).
    g_remove(path);
    g_free(path);
    g_rmdir(dir);
    g_free(dir);
}

static GPtrArray *fake_fanin(const gchar *path, gpointer user_data)
{
    (void)user_data;
    GPtrArray *cone = g_ptr_array_new_with_free_func(g_free);
    if (strcmp(path, "tb.dut.y") == 0) {
        g_ptr_array_add(cone, g_strdup("tb.dut.a"));
        g_ptr_array_add(cone, g_strdup("tb.dut.b"));
        g_ptr_array_add(cone, g_strdup("tb.not_changed_here"));
    } else if (strcmp(path, "tb.dut.b") == 0) {
        g_ptr_array_add(cone, g_strdup("tb.dut.a"));
    } else if (strcmp(path, "tb.bus") == 0) {
        return NULL; // not in the netlist
    }
    return cone;
}

static void test_causality(void)
{
    gchar *dir = g_dir_make_tmp("evseq-XXXXXX", NULL);
    gchar *path = write_tmp(dir, "t.vcd", VCD_TEXT);
    EventSeqResult *r = event_sequence_scan(path, 5, NULL);

    event_sequence_assign_causality(r, fake_fanin, NULL, 0);
    g_assert_true(r->causality_known);

    const EventSeqChange *a = find(r, "tb.dut.a");
    const EventSeqChange *b = find(r, "tb.dut.b");
    const EventSeqChange *y = find(r, "tb.dut.y");
    const EventSeqChange *bus = find(r, "tb.bus");

    g_assert_cmpint(a->level, ==, 0);
    g_assert_cmpint(b->level, ==, 1); // a -> b
    g_assert_cmpint(y->level, ==, 2); // a,b -> y
    g_assert_cmpuint(y->causes->len, ==, 2); // the unchanged signal is dropped
    g_assert_cmpint(bus->level, ==, -1); // unknown, not a fake 0
    g_assert_nonnull(r->warning);

    // y was dumped (seq 2) before its causes a (seq 3) and b (seq 5).
    g_assert_true(y->dumped_before_cause);
    g_assert_false(a->dumped_before_cause);

    event_sequence_result_free(r);

    // Cap: only the first signal is queried, the rest stay unranked.
    r = event_sequence_scan(path, 5, NULL);
    event_sequence_assign_causality(r, fake_fanin, NULL, 1);
    g_assert_nonnull(r->warning);
    g_assert_cmpint(find(r, "tb.dut.y")->level, ==, -1);
    event_sequence_result_free(r);

    g_remove(path);
    g_free(path);
    g_rmdir(dir);
    g_free(dir);
}

// y's only fan-in is clk, reported under clk's ALIAS name; and only the
// alias name of `a` resolves in the "netlist" (the primary name is unknown).
static GPtrArray *alias_fanin(const gchar *path, gpointer user_data)
{
    (void)user_data;
    GPtrArray *cone = g_ptr_array_new_with_free_func(g_free);
    if (strcmp(path, "tb.dut.y") == 0) {
        g_ptr_array_add(cone, g_strdup("tb.clk"));
    } else if (strcmp(path, "tb.dut.clk_alias") == 0) {
        g_ptr_array_unref(cone);
        return NULL; // primary name unknown to the netlist...
    } else if (strcmp(path, "tb.clk") != 0) {
        return cone;
    }
    return cone; // ...but its alias resolves, with an empty cone
}

static void test_alias_matching(void)
{
    gchar *dir = g_dir_make_tmp("evseq-XXXXXX", NULL);
    gchar *path = write_tmp(dir, "t.vcd", VCD_TEXT);
    EventSeqResult *r = event_sequence_scan(path, 5, NULL);

    event_sequence_assign_causality(r, alias_fanin, NULL, 0);

    const EventSeqChange *clk = find(r, "tb.dut.clk_alias");
    const EventSeqChange *y = find(r, "tb.dut.y");
    g_assert_cmpint(clk->level, ==, 0); // resolved via its alias
    g_assert_cmpuint(y->causes->len, ==, 1);
    g_assert_true(g_ptr_array_index(y->causes, 0) == clk); // matched by alias name
    g_assert_cmpint(y->level, ==, 1);

    event_sequence_result_free(r);
    g_remove(path);
    g_free(path);
    g_rmdir(dir);
    g_free(dir);
}

static GPtrArray *cyclic_fanin(const gchar *path, gpointer user_data)
{
    (void)user_data;
    GPtrArray *cone = g_ptr_array_new_with_free_func(g_free);
    g_ptr_array_add(cone, g_strdup(strcmp(path, "tb.dut.a") == 0 ? "tb.dut.b" : "tb.dut.a"));
    return cone;
}

static void test_cycle_terminates(void)
{
    gchar *dir = g_dir_make_tmp("evseq-XXXXXX", NULL);
    gchar *path = write_tmp(dir, "t.vcd", VCD_TEXT);
    EventSeqResult *r = event_sequence_scan(path, 5, NULL);

    event_sequence_assign_causality(r, cyclic_fanin, NULL, 0);
    g_assert_cmpint(find(r, "tb.dut.a")->level, >=, 0);
    g_assert_cmpint(find(r, "tb.dut.b")->level, >=, 0);

    event_sequence_result_free(r);
    g_remove(path);
    g_free(path);
    g_rmdir(dir);
    g_free(dir);
}

static void test_gzip(void)
{
    gchar *dir = g_dir_make_tmp("evseq-XXXXXX", NULL);
    gchar *path = write_tmp(dir, "t.vcd", VCD_TEXT);
    gchar *cmd = g_strdup_printf("gzip -f '%s'", path);
    if (system(cmd) != 0) {
        g_test_skip("gzip not available");
        g_free(cmd);
        g_free(path);
        g_free(dir);
        return;
    }
    g_free(cmd);

    gchar *gz = g_strconcat(path, ".gz", NULL);
    GError *error = NULL;
    EventSeqResult *r = event_sequence_scan(gz, 5, &error);
    g_assert_no_error(error);
    g_assert_cmpuint(r->changes->len, ==, 5);
    g_assert_true(r->order_known);
    event_sequence_result_free(r);

    g_remove(gz);
    g_rmdir(dir);
    g_free(gz);
    g_free(path);
    g_free(dir);
}

static void test_fst(void)
{
    gchar *dir = g_dir_make_tmp("evseq-XXXXXX", NULL);
    gchar *path = g_build_filename(dir, "t.fst", NULL);

    fstWriterContext *w = fstWriterCreate(path, 1);
    g_assert_nonnull(w);
    fstWriterSetTimescale(w, -9);
    fstWriterSetScope(w, FST_ST_VCD_MODULE, "tb", NULL);
    fstHandle clk = fstWriterCreateVar(
        w, FST_VT_VCD_WIRE, FST_VD_IMPLICIT, 1, "clk", 0);
    fstHandle bus = fstWriterCreateVar(
        w, FST_VT_VCD_WIRE, FST_VD_IMPLICIT, 8, "bus [7:0]", 0);
    fstWriterSetUpscope(w);
    fstWriterEmitValueChange(w, clk, "0");
    fstWriterEmitValueChange(w, bus, "00000000");
    fstWriterEmitTimeChange(w, 5);
    fstWriterEmitValueChange(w, clk, "1");
    fstWriterEmitValueChange(w, bus, "00001111");
    fstWriterEmitTimeChange(w, 10);
    fstWriterEmitValueChange(w, clk, "0");
    fstWriterClose(w);

    GError *error = NULL;
    EventSeqResult *r = event_sequence_scan(path, 5, &error);
    g_assert_no_error(error);
    g_assert_nonnull(r);
    g_assert_false(r->order_known); // FST can't give dump order
    g_assert_cmpuint(r->changes->len, ==, 2);
    const EventSeqChange *c = find(r, "tb.clk");
    g_assert_nonnull(c);
    g_assert_cmpuint(c->seq, ==, 0);
    g_assert_cmpstr(c->value, ==, "1");
    g_assert_cmpstr(find(r, "tb.bus")->value, ==, "b00001111");
    event_sequence_result_free(r);

    r = event_sequence_scan(path, 10, &error);
    g_assert_no_error(error);
    g_assert_cmpuint(r->changes->len, ==, 1);
    event_sequence_result_free(r);

    g_remove(path);
    g_rmdir(dir);
    g_free(path);
    g_free(dir);
}

static void test_errors(void)
{
    GError *error = NULL;
    g_assert_null(event_sequence_scan("-vcd", 0, &error));
    g_assert_error(error, EVENT_SEQUENCE_ERROR, EVENT_SEQUENCE_ERROR_UNSUPPORTED);
    g_clear_error(&error);

    g_assert_null(event_sequence_scan("/nonexistent/x.vcd", 0, &error));
    g_assert_error(error, EVENT_SEQUENCE_ERROR, EVENT_SEQUENCE_ERROR_IO);
    g_clear_error(&error);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    g_test_add_func("/event-sequence/vcd-order", test_vcd_order);
    g_test_add_func("/event-sequence/causality", test_causality);
    g_test_add_func("/event-sequence/alias-matching", test_alias_matching);
    g_test_add_func("/event-sequence/cycle", test_cycle_terminates);
    g_test_add_func("/event-sequence/gzip", test_gzip);
    g_test_add_func("/event-sequence/fst", test_fst);
    g_test_add_func("/event-sequence/errors", test_errors);
    return g_test_run();
}

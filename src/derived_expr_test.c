#include <glib.h>
#include <string.h>

#include "derived_expr.h"

/* ---------------------------------------------------------- helpers -- */

// "0:1010 5:11x0" -> series of the given width: at t=0 value 1010, ...
static DerivedSeries *ser(guint width, const gchar *spec)
{
    DerivedSeries *s = derived_series_new(width);
    gchar **parts = g_strsplit(spec, " ", -1);
    for (guint i = 0; parts[i] != NULL; i++) {
        if (parts[i][0] == '\0') {
            continue;
        }
        gchar *colon = strchr(parts[i], ':');
        g_assert_nonnull(colon);
        gint64 t = g_ascii_strtoll(parts[i], NULL, 10);
        const gchar *v = colon + 1;
        g_assert_cmpuint(strlen(v), ==, width);
        guint8 *bits = g_new(guint8, width);
        for (guint k = 0; k < width; k++) {
            const gchar *p = strchr("01xz", v[k]);
            g_assert_nonnull(p);
            bits[k] = (guint8)(p - "01xz");
        }
        derived_series_append(s, t, bits);
        g_free(bits);
    }
    g_strfreev(parts);
    return s;
}

static gchar *bits_str(const DerivedSeries *s, guint i)
{
    gchar *out = g_malloc(s->width + 1);
    for (guint k = 0; k < s->width; k++) {
        out[k] = "01xz"[s->bits[(gsize)i * s->width + k]];
    }
    out[s->width] = '\0';
    return out;
}

// Parses `expr` with a/b/c as plain names and returns the value at the last
// change, as MSB-first text. a/b/c are "t:bits" specs, widths from the text.
static gchar *eval_last(const gchar *expr, const gchar *aspec, const gchar *bspec, const gchar *cspec)
{
    GError *err = NULL;
    DerivedExpr *e = derived_expr_parse(expr, NULL, NULL, &err);
    g_assert_no_error(err);
    g_assert_nonnull(e);

    const gchar *specs[3] = {aspec, bspec, cspec};
    const gchar *names[3] = {"a", "b", "c"};
    DerivedSeries *series[3] = {NULL, NULL, NULL};
    DerivedInput inputs[3];
    guint n = 0;
    GPtrArray *refs = derived_expr_references(e);
    for (guint r = 0; r < refs->len; r++) {
        for (guint k = 0; k < 3; k++) {
            if (g_str_equal(g_ptr_array_index(refs, r), names[k])) {
                g_assert_nonnull(specs[k]);
                const gchar *colon = strchr(specs[k], ':');
                series[k] = ser((guint)strlen(colon + 1), specs[k]);
                inputs[n].name = names[k];
                inputs[n].series = series[k];
                n++;
            }
        }
    }
    g_ptr_array_unref(refs);

    DerivedSeries *out = derived_expr_evaluate(e, inputs, n, &err);
    g_assert_no_error(err);
    g_assert_nonnull(out);
    g_assert_cmpuint(out->n, >, 0);
    gchar *r = bits_str(out, out->n - 1);

    derived_series_free(out);
    for (guint k = 0; k < 3; k++) {
        derived_series_free(series[k]);
    }
    derived_expr_free(e);
    return r;
}

#define CHECK(expr, a, b, c, want)                         \
    G_STMT_START                                           \
    {                                                      \
        gchar *got = eval_last(expr, a, b, c);             \
        if (!g_str_equal(got, want)) {                     \
            g_test_message("%s => %s, wanted %s", expr, got, want); \
        }                                                  \
        g_assert_cmpstr(got, ==, want);                    \
        g_free(got);                                       \
    }                                                      \
    G_STMT_END

/* ------------------------------------------------------------ parsing -- */

static gchar *canon(const gchar *text)
{
    GError *err = NULL;
    DerivedExpr *e = derived_expr_parse(text, NULL, NULL, &err);
    g_assert_no_error(err);
    gchar *s = derived_expr_to_string(e);
    derived_expr_free(e);
    return s;
}

static void expect_canon(const gchar *text, const gchar *want)
{
    gchar *got = canon(text);
    if (!g_str_equal(got, want)) {
        g_test_message("%s => %s, wanted %s", text, got, want);
    }
    g_assert_cmpstr(got, ==, want);

    // The canonical text must parse back to itself.
    gchar *again = canon(got);
    g_assert_cmpstr(again, ==, got);
    g_free(again);
    g_free(got);
}

static void test_precedence(void)
{
    expect_canon("a + b * c", "(a + (b * c))");
    expect_canon("(a + b) * c", "((a + b) * c)");
    expect_canon("a - b - c", "((a - b) - c)"); // left associative
    expect_canon("a | b ^ c & d", "(a | (b ^ (c & d)))");
    expect_canon("a & b == c", "(a & (b == c))"); // == binds tighter than &
    expect_canon("a == b < c", "(a == (b < c))");
    expect_canon("a < b << c", "(a < (b << c))");
    expect_canon("a << b + c", "(a << (b + c))");
    expect_canon("a || b && c", "(a || (b && c))");
    expect_canon("a && b | c", "(a && (b | c))");
    expect_canon("a ? b : c ? d : a", "(a ? b : (c ? d : a))");
    expect_canon("a ? b + c : d", "(a ? (b + c) : d)");
    expect_canon("a ^~ b", "(a ~^ b)");
    expect_canon("a ~^ b", "(a ~^ b)");
    expect_canon("a <= b", "(a <= b)");
    expect_canon("a < b", "(a < b)");
    expect_canon("a << 2", "(a << 2'b10)");
}

static void test_unary_and_reductions(void)
{
    expect_canon("~a", "(~a)");
    expect_canon("!a", "(!a)");
    expect_canon("-a", "(-a)");
    expect_canon("&a", "(&a)");
    expect_canon("|a", "(|a)");
    expect_canon("^a", "(^a)");
    expect_canon("~&a", "(~&a)");
    expect_canon("~|a", "(~|a)");
    expect_canon("~^a", "(~^a)");
    expect_canon("^~a", "(~^a)");
    expect_canon("~~a", "(~(~a))");
    expect_canon("a & ~b", "(a & (~b))");
    expect_canon("a && !b", "(a && (!b))");
    expect_canon("&a == 1", "((&a) == 1'b1)"); // reduction applies to a only
    expect_canon("+a", "a"); // unary plus is a no-op
    expect_canon("a | | b", "(a | (|b))");
    expect_canon("a &&& b", "(a && (&b))"); // logical-and with a reduction
}

static void test_concat_select_literal(void)
{
    expect_canon("{a, b}", "{a, b}");
    expect_canon("{a,b , c}", "{a, b, c}");
    expect_canon("{3{a}}", "{3{a}}");
    expect_canon("{2{a, b}}", "{2{{a, b}}}");
    expect_canon("a[3]", "a[3]");
    expect_canon("a[7:4]", "a[7:4]");
    expect_canon("a[7:4][1]", "a[7:4][1]");
    expect_canon("(a + b)[3]", "(a + b)[3]");
    expect_canon("{4'b1010, a}", "{4'b1010, a}");
    expect_canon("{8, a}", "{4'b1000, a}"); // a number first, not a replication
    expect_canon("8'hA5", "8'b10100101");
    expect_canon("4'b1x0z", "4'b1x0z");
    expect_canon("'d5", "3'b101");
    expect_canon("255", "8'b11111111");
    expect_canon("0", "1'b0");
    expect_canon("12'o777", "12'b000111111111");
    expect_canon("8'hFx", "8'b1111xxxx");
    expect_canon("4'bx", "4'bxxxx"); // x pads on the left
    expect_canon("4'b1", "4'b0001");
    expect_canon("2'b1111", "2'b11"); // too many digits truncate
    expect_canon("8'd200", "8'b11001000");
    expect_canon("1_0", "4'b1010"); // underscores
}

static void test_names(void)
{
    expect_canon("top.u_counter.count_q + 1", "(top.u_counter.count_q + 1'b1)");
    expect_canon("\"top.bus[7:0]\" & a", "(\"top.bus[7:0]\" & a)");
    expect_canon("\"has\\\"quote\"", "\"has\\\"quote\"");
    expect_canon("a_1$b", "a_1$b");
}

static void expect_syntax_error(const gchar *text)
{
    GError *err = NULL;
    DerivedExpr *e = derived_expr_parse(text, NULL, NULL, &err);
    if (e != NULL) {
        g_test_message("'%s' should not parse", text);
    }
    g_assert_null(e);
    g_assert_nonnull(err);
    g_assert_error(err, DERIVED_EXPR_ERROR, DERIVED_EXPR_ERROR_SYNTAX);
    g_clear_error(&err);
}

static void test_syntax_errors(void)
{
    const gchar *bad[] = {"",       "   ",     "a +",    "+ +",      "(a",      "a)",
                          "a b",    "{",       "{a",     "{a,}",     "a[",      "a[1",
                          "a[x]",   "a[2:5]",  "a ? b",  "a ? b :",  "8'hZZG",  "4'q1",
                          "'",      "8'",      "{0{a}}", "a[b]",     "\"open",                           "#",      "a @ b",   "1 2"};
    for (guint i = 0; i < G_N_ELEMENTS(bad); i++) {
        expect_syntax_error(bad[i]);
    }

    // Position is reported.
    GError *err = NULL;
    g_assert_null(derived_expr_parse("a + * b", NULL, NULL, &err));
    g_assert_nonnull(strstr(err->message, "column"));
    g_clear_error(&err);
}

static gchar *fake_resolver(const gchar *name, gpointer user_data)
{
    (void)user_data;
    static const struct
    {
        const gchar *typed;
        const gchar *canonical;
    } table[] = {
        {"clk", "top.clk"},
        {"cnt", "top.u.cnt"},
        {"cnt[7:0]", "top.u.cnt[7:0]"}, // a declared bus name
        {"data[3]", "top.data[3]"},     // a declared single-bit name
        {"data[3:0]", "top.data[3:0]"},
        {"data", "top.data[3:0]"},
        {"top.bus[7:0]", "top.bus[7:0]"},
    };
    for (guint i = 0; i < G_N_ELEMENTS(table); i++) {
        if (g_str_equal(name, table[i].typed)) {
            return g_strdup(table[i].canonical);
        }
    }
    return NULL;
}

static void test_resolver(void)
{
    GError *err = NULL;
    DerivedExpr *e = derived_expr_parse("clk & cnt[0]", fake_resolver, NULL, &err);
    g_assert_no_error(err);
    // `cnt[0]` isn't a declared name, so it's cnt with a bit select.
    gchar *s = derived_expr_to_string(e);
    g_assert_cmpstr(s, ==, "(top.clk & top.u.cnt[0])");
    g_free(s);
    derived_expr_free(e);

    // A declared bus name wins over name + select.
    e = derived_expr_parse("cnt[7:0] + 1", fake_resolver, NULL, &err);
    g_assert_no_error(err);
    GPtrArray *refs = derived_expr_references(e);
    g_assert_cmpuint(refs->len, ==, 1);
    g_assert_cmpstr(g_ptr_array_index(refs, 0), ==, "top.u.cnt[7:0]");
    g_ptr_array_unref(refs);
    derived_expr_free(e);

    // `data[3]` is declared, so it's the bit signal, not a select of `data`.
    e = derived_expr_parse("data[3]", fake_resolver, NULL, &err);
    g_assert_no_error(err);
    refs = derived_expr_references(e);
    g_assert_cmpstr(g_ptr_array_index(refs, 0), ==, "top.data[3]");
    g_ptr_array_unref(refs);
    gchar *text = derived_expr_to_string(e);
    g_assert_cmpstr(text, ==, "\"top.data[3]\""); // no extra select applied
    g_free(text);
    derived_expr_free(e);

    // Spaces inside the brackets still find the declared name.
    e = derived_expr_parse("cnt[ 7:0 ]", fake_resolver, NULL, &err);
    g_assert_no_error(err);
    derived_expr_free(e);

    // Quoted names go straight to the resolver.
    e = derived_expr_parse("\"top.bus[7:0]\" ^ clk", fake_resolver, NULL, &err);
    g_assert_no_error(err);
    refs = derived_expr_references(e);
    g_assert_cmpuint(refs->len, ==, 2);
    g_ptr_array_unref(refs);
    derived_expr_free(e);

    // Repeated references are listed once.
    e = derived_expr_parse("clk & clk | clk", fake_resolver, NULL, &err);
    g_assert_no_error(err);
    refs = derived_expr_references(e);
    g_assert_cmpuint(refs->len, ==, 1);
    g_ptr_array_unref(refs);
    derived_expr_free(e);

    // Unknown signals are named in the error.
    g_assert_null(derived_expr_parse("clk & nosuch", fake_resolver, NULL, &err));
    g_assert_error(err, DERIVED_EXPR_ERROR, DERIVED_EXPR_ERROR_UNKNOWN_SIGNAL);
    g_assert_nonnull(strstr(err->message, "nosuch"));
    g_clear_error(&err);
    g_assert_null(derived_expr_parse("\"no such\"", fake_resolver, NULL, &err));
    g_assert_error(err, DERIVED_EXPR_ERROR, DERIVED_EXPR_ERROR_UNKNOWN_SIGNAL);
    g_clear_error(&err);
}

/* --------------------------------------------------------- semantics -- */

static void test_bitwise_4state(void)
{
    CHECK("a & b", "0:11x0", "0:1010", NULL, "10x0");
    CHECK("a | b", "0:11x0", "0:1010", NULL, "1110");
    CHECK("a ^ b", "0:1100", "0:10x1", NULL, "01x1");
    CHECK("a ~^ b", "0:1100", "0:1010", NULL, "1001");
    CHECK("~a", "0:10xz", NULL, NULL, "01xx");
    // 0 dominates AND, 1 dominates OR, even against an unknown.
    CHECK("a & b", "0:xxxx", "0:0101", NULL, "0x0x");
    CHECK("a | b", "0:xxxx", "0:0101", NULL, "x1x1");
    CHECK("a & b", "0:zzzz", "0:0000", NULL, "0000");
    // Different widths: the shorter one is zero-extended.
    CHECK("a & b", "0:11111111", "0:101", NULL, "00000101");
    CHECK("a | b", "0:1000", "0:1", NULL, "1001");
    CHECK("a ^ b", "0:1", "0:11", NULL, "10");
}

static void test_logical_and_reduction(void)
{
    CHECK("a && b", "0:1", "0:1", NULL, "1");
    CHECK("a && b", "0:1", "0:0", NULL, "0");
    CHECK("a && b", "0:x", "0:0", NULL, "0"); // false dominates
    CHECK("a && b", "0:x", "0:1", NULL, "x");
    CHECK("a || b", "0:x", "0:1", NULL, "1"); // true dominates
    CHECK("a || b", "0:x", "0:0", NULL, "x");
    CHECK("a || b", "0:0", "0:0", NULL, "0");
    CHECK("!a", "0:0", NULL, NULL, "1");
    CHECK("!a", "0:0100", NULL, NULL, "0"); // any set bit is true
    CHECK("!a", "0:0x00", NULL, NULL, "x");
    CHECK("!a", "0:1x00", NULL, NULL, "0"); // a 1 makes it true regardless
    CHECK("&a", "0:1111", NULL, NULL, "1");
    CHECK("&a", "0:11x1", NULL, NULL, "x");
    CHECK("&a", "0:01x1", NULL, NULL, "0"); // a 0 decides it
    CHECK("|a", "0:00x0", NULL, NULL, "x");
    CHECK("|a", "0:01x0", NULL, NULL, "1");
    CHECK("|a", "0:0000", NULL, NULL, "0");
    CHECK("^a", "0:1101", NULL, NULL, "1");
    CHECK("^a", "0:1100", NULL, NULL, "0");
    CHECK("^a", "0:11x0", NULL, NULL, "x");
    CHECK("~&a", "0:1111", NULL, NULL, "0");
    CHECK("~|a", "0:0000", NULL, NULL, "1");
    CHECK("~^a", "0:1100", NULL, NULL, "1");
}

static void test_arithmetic(void)
{
    CHECK("a + b", "0:1001", "0:1001", NULL, "0010"); // 9+9 wraps to 4 bits
    CHECK("a - b", "0:0011", "0:0101", NULL, "1110"); // 3-5 = 14
    CHECK("a * b", "0:0101", "0:0101", NULL, "1001"); // 25 mod 16
    CHECK("a / b", "0:1001", "0:0010", NULL, "0100");
    CHECK("a % b", "0:1001", "0:0010", NULL, "0001");
    CHECK("a / b", "0:1001", "0:0000", NULL, "xxxx"); // divide by zero
    CHECK("a % b", "0:1001", "0:0000", NULL, "xxxx");
    CHECK("a + b", "0:10x1", "0:0001", NULL, "xxxx"); // any unknown bit
    CHECK("a + b", "0:10z1", "0:0001", NULL, "xxxx");
    CHECK("-a", "0:0001", NULL, NULL, "1111");
    CHECK("-a", "0:0000", NULL, NULL, "0000");
    CHECK("-a", "0:00x1", NULL, NULL, "xxxx");
    // Width is the wider operand; unsized literals are just wide enough.
    CHECK("a + 1", "0:11111111", NULL, NULL, "00000000");
    CHECK("a + 1", "0:00001001", NULL, NULL, "00001010");
    CHECK("a + 16", "0:00000001", NULL, NULL, "00010001");
    CHECK("a + b", "0:0001", "0:11111111", NULL, "00000000");
    // 64-bit arithmetic is exact.
    CHECK("a + 1", "0:1111111111111111111111111111111111111111111111111111111111111110", NULL,
          NULL, "1111111111111111111111111111111111111111111111111111111111111111");
    CHECK("a + 1", "0:1111111111111111111111111111111111111111111111111111111111111111", NULL,
          NULL, "0000000000000000000000000000000000000000000000000000000000000000");
}

static void test_comparison_and_shift(void)
{
    CHECK("a == b", "0:1010", "0:1010", NULL, "1");
    CHECK("a == b", "0:1010", "0:1011", NULL, "0");
    CHECK("a != b", "0:1010", "0:1011", NULL, "1");
    CHECK("a < b", "0:0011", "0:0100", NULL, "1");
    CHECK("a <= b", "0:0100", "0:0100", NULL, "1");
    CHECK("a > b", "0:0100", "0:0011", NULL, "1");
    CHECK("a >= b", "0:0011", "0:0100", NULL, "0");
    CHECK("a == b", "0:10x0", "0:1010", NULL, "x");
    CHECK("a < b", "0:00z0", "0:0100", NULL, "x");
    // Mixed widths compare by value.
    CHECK("a == b", "0:00001010", "0:1010", NULL, "1");
    CHECK("a == 300", "0:11111111", NULL, NULL, "0"); // 255 != 300 (9-bit literal)
    CHECK("a < 300", "0:11111111", NULL, NULL, "1");
    CHECK("a << 2", "0:0011", NULL, NULL, "1100");
    CHECK("a << 1", "0:1100", NULL, NULL, "1000"); // high bit shifted out
    CHECK("a >> 1", "0:1100", NULL, NULL, "0110");
    CHECK("a >> 4", "0:1111", NULL, NULL, "0000"); // amount >= width
    CHECK("a << 9", "0:1111", NULL, NULL, "0000");
    CHECK("a << b", "0:0001", "0:0010", NULL, "0100");
    CHECK("a << b", "0:0001", "0:00x0", NULL, "xxxx"); // unknown amount
    CHECK("a << 1", "0:1x00", NULL, NULL, "x000"); // unknown bits just move
    CHECK("a >> 1", "0:00x1", NULL, NULL, "000x");
}

static void test_ternary_concat_select(void)
{
    CHECK("c ? a : b", "0:1010", "0:0101", "0:1", "1010");
    CHECK("c ? a : b", "0:1010", "0:0101", "0:0", "0101");
    CHECK("c ? a : b", "0:1010", "0:1001", "0:x", "10xx"); // equal bits survive
    CHECK("c ? a : b", "0:1010", "0:1010", "0:x", "1010");
    CHECK("c ? a : b", "0:10x0", "0:10x0", "0:x", "10x0"); // equal known bits survive; x stays x
    CHECK("c ? a : b", "0:1010", "0:0101", "0:0010", "1010"); // any set bit is true
    CHECK("c ? a : b", "0:11", "0:0001", "0:1", "0011"); // result is the wider arm
    CHECK("{a, b}", "0:10", "0:011", NULL, "10011");
    CHECK("{a, 2'b01, a}", "0:1", NULL, NULL, "1011");
    CHECK("{3{a}}", "0:10", NULL, NULL, "101010");
    CHECK("{2{a, b}}", "0:1", "0:0", NULL, "1010");
    CHECK("a[3]", "0:1000", NULL, NULL, "1");
    CHECK("a[0]", "0:1000", NULL, NULL, "0");
    CHECK("a[2:1]", "0:0110", NULL, NULL, "11");
    CHECK("a[3:0]", "0:1010", NULL, NULL, "1010");
    CHECK("a[5]", "0:1010", NULL, NULL, "x"); // out of range reads as x
    CHECK("a[5:2]", "0:1010", NULL, NULL, "xx10");
    CHECK("(a + b)[3]", "0:1000", "0:1000", NULL, "0");
    CHECK("a[7:4][1]", "0:01000000", NULL, NULL, "0");
    CHECK("a[7:4][2]", "0:01000000", NULL, NULL, "1");
    CHECK("8'hA5", NULL, NULL, NULL, "10100101");
    CHECK("4'b1x0z", NULL, NULL, NULL, "1x0z");
}

static void test_width_errors(void)
{
    GError *err = NULL;
    DerivedExpr *e = derived_expr_parse("a + b", NULL, NULL, &err);
    DerivedSeries *wide = ser(65, "0:11111111111111111111111111111111111111111111111111111111111111111");
    DerivedSeries *narrow = ser(4, "0:0001");
    DerivedInput inputs[] = {{"a", wide}, {"b", narrow}};
    guint w = 0;
    g_assert_false(derived_expr_width(e, inputs, 2, &w, &err));
    g_assert_error(err, DERIVED_EXPR_ERROR, DERIVED_EXPR_ERROR_WIDTH);
    g_assert_nonnull(strstr(err->message, "64"));
    g_clear_error(&err);
    g_assert_null(derived_expr_evaluate(e, inputs, 2, &err));
    g_assert_error(err, DERIVED_EXPR_ERROR, DERIVED_EXPR_ERROR_WIDTH);
    g_clear_error(&err);
    derived_expr_free(e);

    // Bitwise, concat and select on wide signals are fine.
    e = derived_expr_parse("(a & a)[64:60]", NULL, NULL, &err);
    g_assert_no_error(err);
    g_assert_true(derived_expr_width(e, inputs, 2, &w, &err));
    g_assert_cmpuint(w, ==, 5);
    derived_expr_free(e);

    // A missing input is reported, not crashed on.
    e = derived_expr_parse("a & nosuch", NULL, NULL, &err);
    DerivedInput only_a[] = {{"a", narrow}};
    g_assert_null(derived_expr_evaluate(e, only_a, 1, &err));
    g_assert_error(err, DERIVED_EXPR_ERROR, DERIVED_EXPR_ERROR_UNKNOWN_SIGNAL);
    g_clear_error(&err);
    derived_expr_free(e);

    derived_series_free(wide);
    derived_series_free(narrow);
}

/* ------------------------------------------------------- time merging -- */

static void test_time_merge(void)
{
    GError *err = NULL;
    DerivedExpr *e = derived_expr_parse("a & b", NULL, NULL, &err);
    g_assert_no_error(err);

    // a: 0 at 0, 1 at 10, 0 at 30.   b: 1 at 5, 0 at 20.
    DerivedSeries *a = ser(1, "0:0 10:1 30:0");
    DerivedSeries *b = ser(1, "5:1 20:0");
    DerivedInput in[] = {{"a", a}, {"b", b}};
    DerivedSeries *out = derived_expr_evaluate(e, in, 2, &err);
    g_assert_no_error(err);

    // t=0: a=0, b still unknown -> 0 & x = 0. t=5: 0&1 = 0 (unchanged, dropped).
    // t=10: 1&1 = 1. t=20: 1&0 = 0. t=30: 0&0 = 0 (unchanged, dropped).
    g_assert_cmpuint(out->n, ==, 3);
    g_assert_cmpint(out->times[0], ==, 0);
    g_assert_cmpint(out->times[1], ==, 10);
    g_assert_cmpint(out->times[2], ==, 20);
    gchar *s0 = bits_str(out, 0), *s1 = bits_str(out, 1), *s2 = bits_str(out, 2);
    g_assert_cmpstr(s0, ==, "0");
    g_assert_cmpstr(s1, ==, "1");
    g_assert_cmpstr(s2, ==, "0");
    g_free(s0);
    g_free(s1);
    g_free(s2);
    derived_series_free(out);

    // Before an input's first change it reads as X: the result starts X.
    derived_expr_free(e);
    e = derived_expr_parse("a | b", NULL, NULL, &err);
    DerivedSeries *a2 = ser(1, "0:0 10:1");
    DerivedSeries *b2 = ser(1, "15:0");
    DerivedInput in2[] = {{"a", a2}, {"b", b2}};
    out = derived_expr_evaluate(e, in2, 2, &err);
    g_assert_cmpuint(out->n, ==, 2); // t=15's 1|0 is still 1: collapsed
    s0 = bits_str(out, 0);
    g_assert_cmpstr(s0, ==, "x"); // 0 | x at t=0
    g_free(s0);
    s1 = bits_str(out, 1);
    g_assert_cmpstr(s1, ==, "1"); // 1 | x = 1
    g_free(s1);
    g_assert_cmpint(out->times[1], ==, 10);
    derived_series_free(out);

    // Simultaneous changes produce ONE evaluation, never a glitch from
    // applying them one at a time.
    derived_expr_free(e);
    e = derived_expr_parse("a ^ b", NULL, NULL, &err);
    DerivedSeries *a3 = ser(1, "0:0 10:1");
    DerivedSeries *b3 = ser(1, "0:0 10:1");
    DerivedInput in3[] = {{"a", a3}, {"b", b3}};
    out = derived_expr_evaluate(e, in3, 2, &err);
    g_assert_cmpuint(out->n, ==, 1); // 0^0 and 1^1 are both 0: no change at 10
    derived_series_free(out);

    derived_series_free(a); derived_series_free(b);
    derived_series_free(a2); derived_series_free(b2);
    derived_series_free(a3); derived_series_free(b3);
    derived_expr_free(e);
}

static void test_constant_expression(void)
{
    GError *err = NULL;
    DerivedExpr *e = derived_expr_parse("8'd5 + 3", NULL, NULL, &err);
    g_assert_no_error(err);
    DerivedSeries *out = derived_expr_evaluate(e, NULL, 0, &err);
    g_assert_no_error(err);
    g_assert_cmpuint(out->n, ==, 1);
    g_assert_cmpint(out->times[0], ==, 0);
    gchar *s = bits_str(out, 0);
    g_assert_cmpstr(s, ==, "00001000");
    g_free(s);
    derived_series_free(out);
    derived_expr_free(e);
}

static void test_empty_input_series(void)
{
    // A signal with no changes at all stays unknown; the result has no
    // history rather than inventing one.
    GError *err = NULL;
    DerivedExpr *e = derived_expr_parse("a & b", NULL, NULL, &err);
    DerivedSeries *a = ser(1, "");
    DerivedSeries *b = ser(1, "");
    DerivedInput in[] = {{"a", a}, {"b", b}};
    DerivedSeries *out = derived_expr_evaluate(e, in, 2, &err);
    g_assert_no_error(err);
    g_assert_cmpuint(out->n, ==, 1); // treated as constant-with-unknowns at t=0
    derived_series_free(out);
    derived_series_free(a);
    derived_series_free(b);
    derived_expr_free(e);
}

static void test_large_history(void)
{
    // 300k changes on each of two inputs, offset so every one is a distinct
    // time: exercises the merge's cost and the series growth.
    const guint n = 300000;
    DerivedSeries *a = derived_series_new(8);
    DerivedSeries *b = derived_series_new(8);
    for (guint i = 0; i < n; i++) {
        guint8 va[8], vb[8];
        for (guint k = 0; k < 8; k++) {
            va[k] = (guint8)(((i >> (7 - k)) & 1) ? DERIVED_BIT_1 : DERIVED_BIT_0);
            vb[k] = (guint8)((((i * 3) >> (7 - k)) & 1) ? DERIVED_BIT_1 : DERIVED_BIT_0);
        }
        derived_series_append(a, (gint64)i * 2, va);
        derived_series_append(b, (gint64)i * 2 + 1, vb);
    }
    GError *err = NULL;
    DerivedExpr *e = derived_expr_parse("a + b", NULL, NULL, &err);
    DerivedInput in[] = {{"a", a}, {"b", b}};
    gint64 start = g_get_monotonic_time();
    DerivedSeries *out = derived_expr_evaluate(e, in, 2, &err);
    gint64 elapsed = g_get_monotonic_time() - start;
    g_assert_no_error(err);
    g_assert_cmpuint(out->n, >, n); // roughly one per input change
    for (guint i = 1; i < out->n; i++) {
        g_assert_cmpint(out->times[i], >, out->times[i - 1]);
    }
    g_test_message("evaluated %u+%u changes in %.0f ms", n, n, elapsed / 1000.0);
    g_assert_cmpint(elapsed, <, 5 * G_USEC_PER_SEC);

    derived_series_free(out);
    derived_series_free(a);
    derived_series_free(b);
    derived_expr_free(e);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    g_test_add_func("/derived/precedence", test_precedence);
    g_test_add_func("/derived/unary-and-reductions", test_unary_and_reductions);
    g_test_add_func("/derived/concat-select-literal", test_concat_select_literal);
    g_test_add_func("/derived/names", test_names);
    g_test_add_func("/derived/syntax-errors", test_syntax_errors);
    g_test_add_func("/derived/resolver", test_resolver);
    g_test_add_func("/derived/bitwise-4state", test_bitwise_4state);
    g_test_add_func("/derived/logical-and-reduction", test_logical_and_reduction);
    g_test_add_func("/derived/arithmetic", test_arithmetic);
    g_test_add_func("/derived/comparison-and-shift", test_comparison_and_shift);
    g_test_add_func("/derived/ternary-concat-select", test_ternary_concat_select);
    g_test_add_func("/derived/width-errors", test_width_errors);
    g_test_add_func("/derived/time-merge", test_time_merge);
    g_test_add_func("/derived/constant", test_constant_expression);
    g_test_add_func("/derived/empty-input", test_empty_input_series);
    g_test_add_func("/derived/large-history", test_large_history);
    return g_test_run();
}

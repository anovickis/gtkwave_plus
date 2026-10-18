#include <glib.h>
#include <string.h>

#include "struct_fields.h"

static GPtrArray *parse_ok(const gchar *spec, guint width, guint *unused)
{
    GError *err = NULL;
    GPtrArray *f = struct_fields_parse(spec, width, unused, &err);
    if (f == NULL) {
        g_test_message("'%s' (width %u): %s", spec, width, err->message);
    }
    g_assert_no_error(err);
    g_assert_nonnull(f);
    return f;
}

static void expect_field(GPtrArray *f, guint i, const gchar *name, guint msb, guint lsb,
                         gboolean padding)
{
    g_assert_cmpuint(f->len, >, i);
    const StructField *x = g_ptr_array_index(f, i);
    if (!g_str_equal(x->name, name) || x->msb != msb || x->lsb != lsb || x->padding != padding) {
        g_test_message("field %u is %s[%u:%u]%s, wanted %s[%u:%u]%s", i, x->name, x->msb, x->lsb,
                       x->padding ? " pad" : "", name, msb, lsb, padding ? " pad" : "");
    }
    g_assert_cmpstr(x->name, ==, name);
    g_assert_cmpuint(x->msb, ==, msb);
    g_assert_cmpuint(x->lsb, ==, lsb);
    g_assert_cmpint(x->padding, ==, padding);
}

static void expect_error(const gchar *spec, guint width, StructFieldsError code, const gchar *needle)
{
    GError *err = NULL;
    GPtrArray *f = struct_fields_parse(spec, width, NULL, &err);
    if (f != NULL) {
        g_test_message("'%s' (width %u) should not parse", spec, width);
    }
    g_assert_null(f);
    g_assert_error(err, STRUCT_FIELDS_ERROR, (gint)code);
    if (strstr(err->message, needle) == NULL) {
        g_test_message("'%s': error was \"%s\", wanted it to mention \"%s\"", spec, err->message, needle);
    }
    g_assert_nonnull(strstr(err->message, needle));
    g_clear_error(&err);
}

static void test_sequential(void)
{
    guint unused = 99;
    // 13-bit packed struct, first field in the most significant bits.
    GPtrArray *f = parse_ok("op:4 imm:8 valid:1", 13, &unused);
    g_assert_cmpuint(f->len, ==, 3);
    expect_field(f, 0, "op", 12, 9, FALSE);
    expect_field(f, 1, "imm", 8, 1, FALSE);
    expect_field(f, 2, "valid", 0, 0, FALSE);
    g_assert_cmpuint(unused, ==, 0);
    g_ptr_array_unref(f);

    // Separators: spaces, commas, newlines, in any mix.
    f = parse_ok("  a:2,b:3 ,\n c:1\n", 8, &unused);
    g_assert_cmpuint(f->len, ==, 3);
    expect_field(f, 0, "a", 7, 6, FALSE);
    expect_field(f, 1, "b", 5, 3, FALSE);
    expect_field(f, 2, "c", 2, 2, FALSE);
    g_assert_cmpuint(unused, ==, 2); // bits 1..0 aren't covered
    g_ptr_array_unref(f);

    // Padding takes bits but is flagged so it isn't shown.
    f = parse_ok("op:4 _:3 imm:8", 15, &unused);
    expect_field(f, 1, "_", 10, 8, TRUE);
    expect_field(f, 2, "imm", 7, 0, FALSE);
    g_assert_cmpuint(unused, ==, 0);
    g_ptr_array_unref(f);

    // `_` may repeat (it's not a name); real names may not.
    f = parse_ok("_:1 a:2 _:1 b:4", 8, NULL);
    g_assert_cmpuint(f->len, ==, 4);
    g_ptr_array_unref(f);

    // Names: letters, digits, underscore.
    f = parse_ok("_r1:1 Field_2:3", 4, NULL);
    expect_field(f, 0, "_r1", 3, 3, FALSE);
    g_ptr_array_unref(f);

    // A field the full width of the bus.
    f = parse_ok("all:32", 32, &unused);
    expect_field(f, 0, "all", 31, 0, FALSE);
    g_ptr_array_unref(f);
}

static void test_explicit(void)
{
    guint unused = 99;
    GPtrArray *f = parse_ok("op[12:9] imm[8:1] valid[0]", 13, &unused);
    expect_field(f, 0, "op", 12, 9, FALSE);
    expect_field(f, 1, "imm", 8, 1, FALSE);
    expect_field(f, 2, "valid", 0, 0, FALSE);
    g_assert_cmpuint(unused, ==, 0);
    g_ptr_array_unref(f);

    // Given in any order; always returned most significant first.
    f = parse_ok("valid[0] op[12:9] imm[8:1]", 13, NULL);
    expect_field(f, 0, "op", 12, 9, FALSE);
    expect_field(f, 2, "valid", 0, 0, FALSE);
    g_ptr_array_unref(f);

    // Gaps are fine (and reported); a single bit can be written [n].
    f = parse_ok("hi[15:12] flag[3]", 16, &unused);
    g_assert_cmpuint(unused, ==, 11); // 16 - 4 - 1
    expect_field(f, 1, "flag", 3, 3, FALSE);
    g_ptr_array_unref(f);

    f = parse_ok("a[7:4] _[3:2] b[1:0]", 8, NULL);
    expect_field(f, 1, "_", 3, 2, TRUE);
    g_ptr_array_unref(f);
}

static void test_describe(void)
{
    GPtrArray *f = parse_ok("op:4 imm:8 valid:1", 13, NULL);
    gchar *d = struct_fields_describe(f);
    g_assert_cmpstr(d, ==, "op  [12:9]  4 bits\nimm  [8:1]  8 bits\nvalid  [0]  1 bit");
    g_free(d);
    g_ptr_array_unref(f);

    f = parse_ok("a:2 _:1", 3, NULL);
    d = struct_fields_describe(f);
    g_assert_nonnull(strstr(d, "(padding)"));
    g_free(d);
    g_ptr_array_unref(f);
}

static void test_errors(void)
{
    // Too wide / out of range.
    expect_error("a:8 b:8", 13, STRUCT_FIELDS_ERROR_RANGE, "add up to 16 bits");
    expect_error("a:14", 13, STRUCT_FIELDS_ERROR_RANGE, "add up to 14");
    expect_error("a[13:10]", 13, STRUCT_FIELDS_ERROR_RANGE, "Bit 13 is outside");
    expect_error("a[5:9]", 13, STRUCT_FIELDS_ERROR_RANGE, "reversed");

    // Overlap, duplicates, mixing.
    expect_error("a[7:4] b[5:2]", 8, STRUCT_FIELDS_ERROR_RANGE, "overlap");
    expect_error("a[3] b[3]", 8, STRUCT_FIELDS_ERROR_RANGE, "overlap");
    expect_error("a:2 a:3", 8, STRUCT_FIELDS_ERROR_SYNTAX, "used twice");
    expect_error("a:2 b[3:0]", 8, STRUCT_FIELDS_ERROR_SYNTAX, "use one style");
    expect_error("a[7:4] b:2", 8, STRUCT_FIELDS_ERROR_SYNTAX, "use one style");

    // Syntax.
    expect_error("", 8, STRUCT_FIELDS_ERROR_SYNTAX, "at least one field");
    expect_error("   ,, \n", 8, STRUCT_FIELDS_ERROR_SYNTAX, "at least one field");
    expect_error("a", 8, STRUCT_FIELDS_ERROR_SYNTAX, "needs a width");
    expect_error("a:", 8, STRUCT_FIELDS_ERROR_SYNTAX, "width of at least 1");
    expect_error("a:0", 8, STRUCT_FIELDS_ERROR_SYNTAX, "at least 1");
    expect_error("a:x", 8, STRUCT_FIELDS_ERROR_SYNTAX, "width");
    expect_error("a[", 8, STRUCT_FIELDS_ERROR_SYNTAX, "bit number");
    expect_error("a[3:", 8, STRUCT_FIELDS_ERROR_SYNTAX, "bit number");
    expect_error("a[3:0", 8, STRUCT_FIELDS_ERROR_SYNTAX, "']'");
    expect_error("9a:2", 8, STRUCT_FIELDS_ERROR_SYNTAX, "field name");
    expect_error("a:2 #b:2", 8, STRUCT_FIELDS_ERROR_SYNTAX, "field name");
    expect_error("_:8", 8, STRUCT_FIELDS_ERROR_SYNTAX, "padding");

    // Degenerate bus.
    expect_error("a:1", 0, STRUCT_FIELDS_ERROR_RANGE, "no bits");

    // NULL text behaves as empty.
    GError *err = NULL;
    g_assert_null(struct_fields_parse(NULL, 8, NULL, &err));
    g_clear_error(&err);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    g_test_add_func("/struct-fields/sequential", test_sequential);
    g_test_add_func("/struct-fields/explicit", test_explicit);
    g_test_add_func("/struct-fields/describe", test_describe);
    g_test_add_func("/struct-fields/errors", test_errors);
    return g_test_run();
}

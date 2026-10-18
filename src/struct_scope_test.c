#include <glib.h>
#include <glib/gstdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "fstapi.h"
#include "struct_scope.h"

static GPtrArray *members_of(const gchar *path, const gchar *scope)
{
    GError *err = NULL;
    GPtrArray *m = struct_scope_members(path, scope, &err);
    if (m == NULL) {
        g_test_message("%s: %s", scope, err->message);
    }
    g_assert_no_error(err);
    g_assert_nonnull(m);
    return m;
}

static void expect_member(GPtrArray *m, guint i, const gchar *name, StructMemberKind kind,
                          guint width)
{
    g_assert_cmpuint(m->len, >, i);
    const StructMember *x = g_ptr_array_index(m, i);
    if (!g_str_equal(x->name, name) || x->kind != kind || x->width != width) {
        g_test_message("member %u is '%s' kind=%d width=%u; wanted '%s' kind=%d width=%u", i,
                       x->name, x->kind, x->width, name, kind, width);
    }
    g_assert_cmpstr(x->name, ==, name);
    g_assert_cmpint(x->kind, ==, kind);
    g_assert_cmpuint(x->width, ==, width);
}

// One shape, checked against every file format: names, kinds, widths and --
// the point of the whole module -- DECLARATION order, which is not the
// alphabetical order gtkwave's own tree keeps.
static void check_design(const gchar *path)
{
    // Root: clk, then four struct-like scopes, then zeta before alpha.
    GPtrArray *m = members_of(path, "t");
    g_assert_cmpuint(m->len, ==, 8);
    expect_member(m, 0, "clk", STRUCT_MEMBER_VAR, 1);
    expect_member(m, 1, "instr", STRUCT_MEMBER_STRUCT, 0);
    expect_member(m, 2, "ctx", STRUCT_MEMBER_STRUCT, 0);
    expect_member(m, 3, "w", STRUCT_MEMBER_UNION, 0);
    expect_member(m, 4, "arr[0]", STRUCT_MEMBER_STRUCT, 0);
    expect_member(m, 5, "sub", STRUCT_MEMBER_SCOPE, 0);
    expect_member(m, 6, "zeta", STRUCT_MEMBER_VAR, 1);
    expect_member(m, 7, "alpha", STRUCT_MEMBER_VAR, 1);
    g_ptr_array_unref(m);

    // A packed struct's fields, MSB first: op is NOT alphabetically first.
    m = members_of(path, "t.instr");
    g_assert_cmpuint(m->len, ==, 3);
    expect_member(m, 0, "op", STRUCT_MEMBER_VAR, 4);
    expect_member(m, 1, "imm", STRUCT_MEMBER_VAR, 8);
    expect_member(m, 2, "valid", STRUCT_MEMBER_VAR, 1);
    g_ptr_array_unref(m);

    // Nested struct: a plain field, then the inner struct, in order.
    m = members_of(path, "t.ctx");
    g_assert_cmpuint(m->len, ==, 2);
    expect_member(m, 0, "mode", STRUCT_MEMBER_VAR, 2);
    expect_member(m, 1, "inst", STRUCT_MEMBER_STRUCT, 0);
    g_ptr_array_unref(m);
    m = members_of(path, "t.ctx.inst");
    g_assert_cmpuint(m->len, ==, 3);
    expect_member(m, 0, "op", STRUCT_MEMBER_VAR, 4);
    g_ptr_array_unref(m);

    // Union containing a field and a nested struct.
    m = members_of(path, "t.w");
    g_assert_cmpuint(m->len, ==, 2);
    expect_member(m, 0, "word", STRUCT_MEMBER_VAR, 16);
    expect_member(m, 1, "bytes", STRUCT_MEMBER_STRUCT, 0);
    g_ptr_array_unref(m);
    m = members_of(path, "t.w.bytes");
    expect_member(m, 0, "hi", STRUCT_MEMBER_VAR, 8);
    expect_member(m, 1, "lo", STRUCT_MEMBER_VAR, 8);
    g_ptr_array_unref(m);

    // A scope name containing brackets (array of structs).
    m = members_of(path, "t.arr[0]");
    g_assert_cmpuint(m->len, ==, 1);
    expect_member(m, 0, "x", STRUCT_MEMBER_VAR, 4);
    g_ptr_array_unref(m);

    // An ordinary module is just a scope; an empty scope has no members.
    m = members_of(path, "t.sub");
    g_assert_cmpuint(m->len, ==, 0);
    g_ptr_array_unref(m);

    // Exact path match only: a prefix of a real scope is not that scope.
    GError *err = NULL;
    g_assert_null(struct_scope_members(path, "t.inst", &err));
    g_assert_error(err, STRUCT_SCOPE_ERROR, STRUCT_SCOPE_ERROR_NOT_FOUND);
    g_clear_error(&err);
    g_assert_null(struct_scope_members(path, "nope", &err));
    g_assert_error(err, STRUCT_SCOPE_ERROR, STRUCT_SCOPE_ERROR_NOT_FOUND);
    g_clear_error(&err);
    g_assert_null(struct_scope_members(path, "t.instr.op", &err)); // a signal, not a scope
    g_assert_error(err, STRUCT_SCOPE_ERROR, STRUCT_SCOPE_ERROR_NOT_FOUND);
    g_clear_error(&err);
}

static gchar *tmp_path(const gchar *name)
{
    gchar *dir = g_dir_make_tmp("structscope-XXXXXX", NULL);
    gchar *path = g_build_filename(dir, name, NULL);
    g_free(dir);
    return path;
}

static void cleanup(const gchar *path)
{
    gchar *dir = g_path_get_dirname(path);
    g_remove(path);
    g_rmdir(dir);
    g_free(dir);
}

static void test_fst(void)
{
    gchar *path = tmp_path("d.fst");
    fstWriterContext *w = fstWriterCreate(path, 1);
    g_assert_nonnull(w);
    fstWriterSetTimescale(w, -9);
#define VAR(name, len) \
    fstWriterCreateVar(w, FST_VT_VCD_WIRE, FST_VD_IMPLICIT, len, name, 0)
    fstWriterSetScope(w, FST_ST_VCD_MODULE, "t", NULL);
    fstHandle h = VAR("clk", 1);
    fstWriterSetScope(w, FST_ST_VCD_STRUCT, "instr", NULL);
    VAR("op [3:0]", 4);
    VAR("imm [7:0]", 8);
    VAR("valid", 1);
    fstWriterSetUpscope(w);
    fstWriterSetScope(w, FST_ST_VCD_STRUCT, "ctx", NULL);
    VAR("mode [1:0]", 2);
    fstWriterSetScope(w, FST_ST_VCD_STRUCT, "inst", NULL);
    VAR("op [3:0]", 4);
    VAR("imm [7:0]", 8);
    VAR("valid", 1);
    fstWriterSetUpscope(w);
    fstWriterSetUpscope(w);
    fstWriterSetScope(w, FST_ST_VCD_UNION, "w", NULL);
    VAR("word [15:0]", 16);
    fstWriterSetScope(w, FST_ST_VCD_STRUCT, "bytes", NULL);
    VAR("hi [7:0]", 8);
    VAR("lo [7:0]", 8);
    fstWriterSetUpscope(w);
    fstWriterSetUpscope(w);
    fstWriterSetScope(w, FST_ST_VCD_STRUCT, "arr[0]", NULL);
    VAR("x [3:0]", 4);
    fstWriterSetUpscope(w);
    fstWriterSetScope(w, FST_ST_VCD_MODULE, "sub", NULL);
    fstWriterSetUpscope(w);
    VAR("zeta", 1);
    VAR("alpha", 1);
    fstWriterSetUpscope(w);
#undef VAR
    fstWriterEmitValueChange(w, h, "0");
    fstWriterEmitTimeChange(w, 1);
    fstWriterEmitValueChange(w, h, "1");
    fstWriterClose(w);

    check_design(path);
    cleanup(path);
    g_free(path);
}

static const gchar *VCD =
    "$timescale 1ns $end\n"
    "$scope module t $end\n"
    "$var wire 1 ! clk $end\n"
    "$scope struct instr $end\n"
    "$var wire 4 \" op [3:0] $end\n"
    "$var wire 8 # imm [7:0] $end\n"
    "$var wire 1 $ valid $end\n"
    "$upscope $end\n"
    "$scope struct ctx $end\n"
    "$var wire 2 % mode [1:0] $end\n"
    "$scope struct inst $end\n"
    "$var wire 4 & op [3:0] $end\n"
    "$var wire 8 ' imm [7:0] $end\n"
    "$var wire 1 ( valid $end\n"
    "$upscope $end\n"
    "$upscope $end\n"
    "$scope union w $end\n"
    "$var wire 16 ) word [15:0] $end\n"
    "$scope struct bytes $end\n"
    "$var wire 8 * hi [7:0] $end\n"
    "$var wire 8 + lo [7:0] $end\n"
    "$upscope $end\n"
    "$upscope $end\n"
    "$scope struct arr[0] $end\n"
    "$var wire 4 , x [3:0] $end\n"
    "$upscope $end\n"
    "$scope module sub $end\n"
    "$upscope $end\n"
    "$var wire 1 - zeta $end\n"
    "$var wire 1 . alpha $end\n"
    "$upscope $end\n"
    "$enddefinitions $end\n"
    "#0\n0!\n";

static void test_vcd(void)
{
    gchar *path = tmp_path("d.vcd");
    g_assert_true(g_file_set_contents(path, VCD, -1, NULL));
    check_design(path);
    cleanup(path);
    g_free(path);
}

static void test_vcd_gz(void)
{
    gchar *path = tmp_path("d.vcd");
    g_assert_true(g_file_set_contents(path, VCD, -1, NULL));
    gchar *cmd = g_strdup_printf("gzip -f '%s'", path);
    if (system(cmd) != 0) {
        g_test_skip("gzip not available");
        g_free(cmd);
        g_free(path);
        return;
    }
    g_free(cmd);
    gchar *gz = g_strconcat(path, ".gz", NULL);
    check_design(gz);
    cleanup(gz);
    g_free(gz);
    g_free(path);
}

static void test_errors(void)
{
    GError *err = NULL;
    g_assert_null(struct_scope_members("-vcd", "t", &err));
    g_assert_error(err, STRUCT_SCOPE_ERROR, STRUCT_SCOPE_ERROR_UNSUPPORTED);
    g_clear_error(&err);
    g_assert_null(struct_scope_members("/nonexistent/x.vcd", "t", &err));
    g_assert_error(err, STRUCT_SCOPE_ERROR, STRUCT_SCOPE_ERROR_IO);
    g_clear_error(&err);
    g_assert_null(struct_scope_members(NULL, "t", &err));
    g_clear_error(&err);

    g_assert_true(struct_member_kind_is_container(STRUCT_MEMBER_STRUCT));
    g_assert_true(struct_member_kind_is_container(STRUCT_MEMBER_UNION));
    g_assert_true(struct_member_kind_is_container(STRUCT_MEMBER_RECORD));
    g_assert_false(struct_member_kind_is_container(STRUCT_MEMBER_VAR));
    g_assert_false(struct_member_kind_is_container(STRUCT_MEMBER_SCOPE));
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    g_test_add_func("/struct-scope/fst", test_fst);
    g_test_add_func("/struct-scope/vcd", test_vcd);
    g_test_add_func("/struct-scope/vcd-gz", test_vcd_gz);
    g_test_add_func("/struct-scope/errors", test_errors);
    return g_test_run();
}

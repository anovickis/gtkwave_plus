#include "struct_scope.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>

#include "fstapi.h"

G_DEFINE_QUARK(struct-scope-error-quark, struct_scope_error)

void struct_member_free(StructMember *member)
{
    if (member == NULL) {
        return;
    }
    g_free(member->name);
    g_free(member);
}

gboolean struct_member_kind_is_container(StructMemberKind kind)
{
    return kind == STRUCT_MEMBER_STRUCT || kind == STRUCT_MEMBER_UNION ||
           kind == STRUCT_MEMBER_RECORD;
}

// "op [3:0]" / "q[3]" -> "op" / "q": the range isn't part of the name.
static gchar *strip_range(const gchar *name)
{
    gsize len = strlen(name);
    if (len > 2 && name[len - 1] == ']') {
        const gchar *open = strrchr(name, '[');
        if (open != NULL && open != name) {
            gsize end = (gsize)(open - name);
            while (end > 0 && name[end - 1] == ' ') {
                end--;
            }
            return g_strndup(name, end);
        }
    }
    return g_strdup(name);
}

static gchar *join_path(GPtrArray *scopes)
{
    GString *s = g_string_new(NULL);
    for (guint i = 0; i < scopes->len; i++) {
        if (i > 0) {
            g_string_append_c(s, '.');
        }
        g_string_append(s, g_ptr_array_index(scopes, i));
    }
    return g_string_free(s, FALSE);
}

static StructMember *member_new(const gchar *name, StructMemberKind kind, guint width)
{
    StructMember *m = g_new0(StructMember, 1);
    // A signal's declared name carries its range ("op [3:0]"); a scope's name
    // is used as is -- `arr[0]` is an array element's scope, not a range.
    m->name = kind == STRUCT_MEMBER_VAR ? strip_range(name) : g_strdup(name);
    m->kind = kind;
    m->width = width;
    return m;
}

typedef struct
{
    const gchar *target; // full path of the scope being listed
    GPtrArray *members; // StructMember*, declaration order
    GPtrArray *scopes; // current path components (gchar*)
    gboolean found; // the target scope was seen
} Walk;

static void walk_enter(Walk *w, const gchar *name, StructMemberKind kind)
{
    // Is this scope a direct child of the target? Decide before pushing.
    gchar *parent = join_path(w->scopes);
    if (w->found && g_str_equal(parent, w->target)) {
        g_ptr_array_add(w->members, member_new(name, kind, 0));
    }
    g_free(parent);

    g_ptr_array_add(w->scopes, g_strdup(name));
    gchar *here = join_path(w->scopes);
    if (g_str_equal(here, w->target)) {
        w->found = TRUE;
    }
    g_free(here);
}

static void walk_leave(Walk *w)
{
    if (w->scopes->len > 0) {
        g_ptr_array_set_size(w->scopes, w->scopes->len - 1);
    }
}

static void walk_var(Walk *w, const gchar *name, guint width)
{
    gchar *parent = join_path(w->scopes);
    if (w->found && g_str_equal(parent, w->target)) {
        g_ptr_array_add(w->members, member_new(name, STRUCT_MEMBER_VAR, width));
    }
    g_free(parent);
}

/* ---------------------------------------------------------------- FST -- */

static StructMemberKind fst_kind(unsigned char type)
{
    switch (type) {
        case FST_ST_VCD_STRUCT: return STRUCT_MEMBER_STRUCT;
        case FST_ST_VCD_UNION: return STRUCT_MEMBER_UNION;
        case FST_ST_VHDL_RECORD: return STRUCT_MEMBER_RECORD;
        default: return STRUCT_MEMBER_SCOPE;
    }
}

static gboolean walk_fst(const gchar *path, Walk *w, GError **error)
{
    fstReaderContext *ctx = fstReaderOpen(path);
    if (ctx == NULL) {
        g_set_error(error, STRUCT_SCOPE_ERROR, STRUCT_SCOPE_ERROR_IO,
                    "Could not open %s as an FST file", path);
        return FALSE;
    }
    struct fstHier *h;
    while ((h = fstReaderIterateHier(ctx)) != NULL) {
        if (h->htyp == FST_HT_SCOPE) {
            walk_enter(w, h->u.scope.name, fst_kind(h->u.scope.typ));
        } else if (h->htyp == FST_HT_UPSCOPE) {
            walk_leave(w);
        } else if (h->htyp == FST_HT_VAR) {
            walk_var(w, h->u.var.name, h->u.var.length);
        }
    }
    fstReaderClose(ctx);
    return TRUE;
}

/* ---------------------------------------------------------------- VCD -- */

static StructMemberKind vcd_kind(const gchar *type)
{
    if (g_ascii_strcasecmp(type, "struct") == 0) {
        return STRUCT_MEMBER_STRUCT;
    }
    if (g_ascii_strcasecmp(type, "union") == 0) {
        return STRUCT_MEMBER_UNION;
    }
    if (g_ascii_strcasecmp(type, "record") == 0) {
        return STRUCT_MEMBER_RECORD;
    }
    return STRUCT_MEMBER_SCOPE;
}

static gboolean walk_vcd(const gchar *path, gboolean gz, Walk *w, GError **error)
{
    FILE *fp;
    if (gz) {
        gchar *quoted = g_shell_quote(path);
        gchar *cmd = g_strdup_printf("gzip -dc %s", quoted);
        fp = popen(cmd, "r");
        g_free(cmd);
        g_free(quoted);
    } else {
        fp = fopen(path, "rb");
    }
    if (fp == NULL) {
        g_set_error(error, STRUCT_SCOPE_ERROR, STRUCT_SCOPE_ERROR_IO, "Could not open %s: %s", path,
                    g_strerror(errno));
        return FALSE;
    }

    GPtrArray *decl = g_ptr_array_new_with_free_func(g_free);
    gboolean in_decl = FALSE;
    gboolean done = FALSE;
    gchar line[4096];

    // The header only: stop at $enddefinitions.
    while (!done && fgets(line, sizeof(line), fp) != NULL) {
        gchar **tokens = g_strsplit_set(line, " \t\r\n", -1);
        for (gchar **tok = tokens; *tok != NULL && !done; tok++) {
            const gchar *t = *tok;
            if (*t == '\0') {
                continue;
            }
            if (!in_decl) {
                if (strcmp(t, "$enddefinitions") == 0) {
                    done = TRUE;
                } else if (t[0] == '$') {
                    in_decl = TRUE;
                    g_ptr_array_set_size(decl, 0);
                    g_ptr_array_add(decl, g_strdup(t));
                }
                continue;
            }
            if (strcmp(t, "$end") != 0) {
                g_ptr_array_add(decl, g_strdup(t));
                continue;
            }
            in_decl = FALSE;
            const gchar *kind = g_ptr_array_index(decl, 0);
            if (strcmp(kind, "$scope") == 0 && decl->len >= 3) {
                walk_enter(w, g_ptr_array_index(decl, 2), vcd_kind(g_ptr_array_index(decl, 1)));
            } else if (strcmp(kind, "$upscope") == 0) {
                walk_leave(w);
            } else if (strcmp(kind, "$var") == 0 && decl->len >= 5) {
                walk_var(w, g_ptr_array_index(decl, 4),
                         (guint)g_ascii_strtoull(g_ptr_array_index(decl, 2), NULL, 10));
            }
        }
        g_strfreev(tokens);
    }

    g_ptr_array_unref(decl);
    if (gz) {
        pclose(fp);
    } else {
        fclose(fp);
    }
    return TRUE;
}

/* ------------------------------------------------------------- public -- */

GPtrArray *struct_scope_members(const gchar *dump_path, const gchar *scope_path, GError **error)
{
    if (dump_path == NULL || strcmp(dump_path, "-vcd") == 0) {
        g_set_error_literal(error, STRUCT_SCOPE_ERROR, STRUCT_SCOPE_ERROR_UNSUPPORTED,
                            "The dump was read from stdin, so its header can't be re-read.");
        return NULL;
    }

    unsigned char magic[4] = {0};
    FILE *fp = fopen(dump_path, "rb");
    if (fp == NULL) {
        g_set_error(error, STRUCT_SCOPE_ERROR, STRUCT_SCOPE_ERROR_IO, "Could not open %s: %s",
                    dump_path, g_strerror(errno));
        return NULL;
    }
    size_t n = fread(magic, 1, sizeof(magic), fp);
    fclose(fp);

    Walk w = {
        .target = scope_path,
        .members = g_ptr_array_new_with_free_func((GDestroyNotify)struct_member_free),
        .scopes = g_ptr_array_new_with_free_func(g_free),
    };

    gboolean ok;
    if (n >= 2 && magic[0] == 0x1f && magic[1] == 0x8b) {
        ok = walk_vcd(dump_path, TRUE, &w, error);
    } else if (n >= 1 && magic[0] == 0) {
        ok = walk_fst(dump_path, &w, error);
    } else if (n >= 4 && memcmp(magic, "GHDL", 4) == 0) {
        g_set_error_literal(error, STRUCT_SCOPE_ERROR, STRUCT_SCOPE_ERROR_UNSUPPORTED,
                            "GHW dumps aren't supported for struct fields.");
        ok = FALSE;
    } else {
        ok = walk_vcd(dump_path, FALSE, &w, error);
    }

    g_ptr_array_unref(w.scopes);
    if (!ok) {
        g_ptr_array_unref(w.members);
        return NULL;
    }
    if (!w.found) {
        g_set_error(error, STRUCT_SCOPE_ERROR, STRUCT_SCOPE_ERROR_NOT_FOUND,
                    "The dump has no scope named '%s'.", scope_path);
        g_ptr_array_unref(w.members);
        return NULL;
    }
    return w.members;
}

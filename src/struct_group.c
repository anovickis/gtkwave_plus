#include <config.h>
#include <gtkwave.h>
#include <string.h>
#include "struct_group.h"
#include "analyzer.h"
#include "globals.h"
#include "menu.h"
#include "status.h"
#include "symbol.h"
#include "treesearch.h"

typedef struct
{
    const gchar *dump_path;
    TraceFlagsType base_flags; // the add-flags in effect on entry (right-justify, ...)
    gboolean packed;
    guint fields; // signals added, for the summary
    guint groups; // groups added
    guint skipped; // members that couldn't be shown
    GString *notes; // things the user should know, shown after the build
    GError *error; // first hard failure
} Builder;

static void note(Builder *b, const gchar *fmt, ...) G_GNUC_PRINTF(2, 3);

static void note(Builder *b, const gchar *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    gchar *msg = g_strdup_vprintf(fmt, ap);
    va_end(ap);
    g_string_append(b->notes, msg);
    g_string_append_c(b->notes, '\n');
    g_free(msg);
}

gboolean struct_group_kind_from_tree_kind(guint tree_kind, StructMemberKind *out)
{
    switch (tree_kind) {
        case GW_TREE_KIND_VCD_ST_STRUCT:
            *out = STRUCT_MEMBER_STRUCT;
            return TRUE;
        case GW_TREE_KIND_VCD_ST_UNION:
            *out = STRUCT_MEMBER_UNION;
            return TRUE;
        case GW_TREE_KIND_VHDL_ST_RECORD:
            *out = STRUCT_MEMBER_RECORD;
            return TRUE;
        default:
            return FALSE;
    }
}

static const gchar *leaf_of(const gchar *path)
{
    const gchar *dot = strrchr(path, '.');
    return dot != NULL ? dot + 1 : path;
}

static GwNode *node_for(const gchar *scope_path, const gchar *member)
{
    gchar *full = g_strconcat(scope_path, ".", member, NULL);
    GwSymbol *sym = gw_dump_file_lookup_symbol(GLOBALS->dump_file, full);
    g_free(full);
    return sym != NULL ? sym->n : NULL;
}

/* ----------------------------------------------------- packed value -- */

typedef struct
{
    GwNode **nodes; // single bits, most significant first
    guint len, cap;
} BitList;

static void bitlist_push(BitList *l, GwNode *n)
{
    if (l->len == l->cap) {
        l->cap = l->cap ? l->cap * 2 : 32;
        l->nodes = g_renew(GwNode *, l->nodes, l->cap);
    }
    l->nodes[l->len++] = n;
}

static void bitlist_append(BitList *dst, const BitList *src)
{
    for (guint i = 0; i < src->len; i++) {
        bitlist_push(dst, src->nodes[i]);
    }
}

// A signal's bits, most significant first. A multi-bit signal is split into
// per-bit nodes the same way Create Bus / Expand do it (gw_node_expand()).
static gboolean node_bits(GwNode *nd, BitList *out)
{
    if (nd->mv.mvlfac != NULL) {
        import_trace(nd); // lazily-loaded formats only read it on demand
    }
    if (nd->head.flags & (GW_HIST_ENT_FLAG_REAL | GW_HIST_ENT_FLAG_STRING)) {
        return FALSE; // a real or string has no bits to concatenate
    }

    GwExpandInfo *e = gw_node_expand(nd);
    if (e == NULL) {
        if (nd->expansion != NULL) {
            nd->expansion->refcnt++;
        }
        bitlist_push(out, nd);
        return TRUE;
    }
    for (gint i = 0; i < e->width; i++) {
        e->narray[i]->expansion->refcnt++;
        bitlist_push(out, e->narray[i]);
    }
    gw_expand_info_free(e);
    return TRUE;
}

static gboolean scope_bits(Builder *b, const gchar *scope_path, StructMemberKind kind,
                           BitList *out);

// One member's bits (a signal, or a nested struct/union's).
static gboolean member_bits(Builder *b, const gchar *scope_path, const StructMember *m,
                            BitList *out)
{
    if (m->kind == STRUCT_MEMBER_VAR) {
        GwNode *nd = node_for(scope_path, m->name);
        return nd != NULL && node_bits(nd, out);
    }
    if (struct_member_kind_is_container(m->kind)) {
        gchar *child = g_strconcat(scope_path, ".", m->name, NULL);
        gboolean ok = scope_bits(b, child, m->kind, out);
        g_free(child);
        return ok;
    }
    return FALSE; // some other scope inside a struct: no defined packing
}

// The packed bits of a struct (members concatenated in declaration order)
// or union (its widest member -- they overlay). FALSE if anything in it
// can't be expressed as bits.
static gboolean scope_bits(Builder *b, const gchar *scope_path, StructMemberKind kind, BitList *out)
{
    GError *err = NULL;
    GPtrArray *members = struct_scope_members(b->dump_path, scope_path, &err);
    if (members == NULL) {
        g_clear_error(&err);
        return FALSE;
    }

    gboolean ok = TRUE;
    if (kind == STRUCT_MEMBER_UNION) {
        BitList widest = {0};
        for (guint i = 0; ok && i < members->len; i++) {
            BitList cur = {0};
            ok = member_bits(b, scope_path, g_ptr_array_index(members, i), &cur);
            if (ok && cur.len > widest.len) {
                g_free(widest.nodes);
                widest = cur;
            } else {
                g_free(cur.nodes);
            }
        }
        if (ok) {
            bitlist_append(out, &widest);
        }
        g_free(widest.nodes);
    } else {
        for (guint i = 0; ok && i < members->len; i++) {
            ok = member_bits(b, scope_path, g_ptr_array_index(members, i), out);
        }
    }
    g_ptr_array_unref(members);
    return ok;
}

// Builds the vector trace's backing data from a bit list, the way
// bus_ops.c's expand_subbus() does, and returns it (or NULL).
static GwBitVector *vector_from_bits(const BitList *bits, const gchar *name)
{
    GwBits *b = calloc_2(1, sizeof(GwBits) + (gsize)bits->len * sizeof(GwNode *));
    for (guint i = 0; i < bits->len; i++) {
        b->nodes[i] = bits->nodes[i];
    }
    b->nnbits = (int)bits->len;
    b->name = malloc_2(strlen(name) + 1);
    strcpy(b->name, name);

    GwBitVector *v = bits2vector(b);
    if (v == NULL) {
        free_2(b->name);
        free_2(b);
        return NULL;
    }
    v->bits = b; /* matches combine_traces()'s ownership hand-off */
    return v;
}

/* ------------------------------------------------------------ emitting -- */

// Flags for the NEXT trace added, the way the save-file loader sets them
// (AddTrace() reads GLOBALS->default_flags). `extra` is ORed onto the flags
// that were in effect when the build started, NOT used in their place: those
// carry settings such as right-justify (the default), and replacing them
// leaves a 13-bit hex bus drawn left-aligned ("1038" instead of "0207").
// The caller's flags are restored when the build ends.
static void set_add_flags(Builder *b, TraceFlagsType extra)
{
    GLOBALS->default_flags = b->base_flags | extra;
}

static void emit_scope(Builder *b, const gchar *scope_path, StructMemberKind kind);

static void emit_member(Builder *b, const gchar *scope_path, const StructMember *m)
{
    if (m->kind == STRUCT_MEMBER_VAR) {
        GwNode *nd = node_for(scope_path, m->name);
        if (nd == NULL) {
            note(b, "'%s.%s' isn't in the loaded dump -- left out.", scope_path, m->name);
            b->skipped++;
            return;
        }
        set_add_flags(b, 0);
        AddNodeTraceReturn(nd, (char *)m->name, NULL);
        b->fields++;
    } else if (struct_member_kind_is_container(m->kind)) {
        gchar *child = g_strconcat(scope_path, ".", m->name, NULL);
        emit_scope(b, child, m->kind);
        g_free(child);
    } else {
        note(b, "'%s' is an ordinary scope inside the struct -- left out.", m->name);
        b->skipped++;
    }
}

static void emit_scope(Builder *b, const gchar *scope_path, StructMemberKind kind)
{
    GError *err = NULL;
    GPtrArray *members = struct_scope_members(b->dump_path, scope_path, &err);
    if (members == NULL) {
        if (b->error == NULL) {
            g_propagate_error(&b->error, err);
        } else {
            g_clear_error(&err);
        }
        return;
    }

    const gchar *name = leaf_of(scope_path);

    // Header: the packed-value bus wearing the group, if wanted and possible;
    // otherwise a plain named group row.
    gboolean composite = FALSE;
    if (b->packed && kind == STRUCT_MEMBER_STRUCT) {
        BitList bits = {0};
        if (scope_bits(b, scope_path, kind, &bits) && bits.len > 0 &&
            bits.len <= BITATTRIBUTES_MAX) {
            GwBitVector *v = vector_from_bits(&bits, scope_path);
            if (v != NULL) {
                set_add_flags(b, TR_GRP_BEGIN);
                AddVector(v, (char *)name);
                free_2(v->bits->name);
                v->bits->name = NULL;
                composite = TRUE;
            }
        } else {
            note(b, "'%s' can't be shown as one packed value (a member isn't plain bits, or it is "
                    "too wide) -- shown as a group of its fields.",
                 name);
        }
        g_free(bits.nodes);
    }
    if (!composite) {
        set_add_flags(b, TR_GRP_BEGIN);
        AddBlankTrace((char *)name);
    }
    b->groups++;

    for (guint i = 0; i < members->len; i++) {
        emit_member(b, scope_path, g_ptr_array_index(members, i));
    }

    // Closing marker. A bus header's is invisible (like Expand's).
    set_add_flags(b, composite ? (TR_GRP_END | TR_COLLAPSED) : TR_GRP_END);
    AddBlankTrace(composite ? (char *)"group_end" : (char *)name);

    g_ptr_array_unref(members);
}

typedef struct
{
    Builder *builder;
    const gchar *scope_path;
    StructMemberKind kind;
} BuildJob;

static void build_fill(gpointer data)
{
    BuildJob *job = data;
    TraceFlagsType saved = GLOBALS->default_flags;
    job->builder->base_flags = saved & ~(TR_GRP_BEGIN | TR_GRP_END | TR_COLLAPSED | TR_CLOSED);

    emit_scope(job->builder, job->scope_path, job->kind);

    GLOBALS->default_flags = saved;
}

gboolean struct_group_add(const gchar *scope_path,
                          StructMemberKind kind,
                          guint mode,
                          gboolean packed_value,
                          GError **error)
{
    if (GLOBALS->dump_file == NULL || GLOBALS->loaded_file_name == NULL) {
        g_set_error_literal(error, STRUCT_SCOPE_ERROR, STRUCT_SCOPE_ERROR_UNSUPPORTED,
                            "No dump file is loaded.");
        return FALSE;
    }
    if (!struct_member_kind_is_container(kind)) {
        g_set_error_literal(error, STRUCT_SCOPE_ERROR, STRUCT_SCOPE_ERROR_UNSUPPORTED,
                            "Select a struct, union or record scope.");
        return FALSE;
    }

    Builder b = {
        .dump_path = GLOBALS->loaded_file_name,
        .packed = packed_value,
        .notes = g_string_new(NULL),
    };
    BuildJob job = {.builder = &b, .scope_path = scope_path, .kind = kind};

    sst_import_in_mode(mode, build_fill, &job);

    // Insert/Replace pasted a sublist into the real one: re-derive each
    // trace's group membership across the seam, as Expand does.
    for (GwTrace *t = GLOBALS->traces.first; t != NULL; t = t->t_next) {
        updateTraceGroup(t);
    }
    GLOBALS->signalwindow_width_dirty = 1;
    redraw_signals_and_waves();

    if (b.error != NULL) {
        g_propagate_error(error, b.error);
        g_string_free(b.notes, TRUE);
        return FALSE;
    }

    gchar *summary = g_strdup_printf("Added struct '%s': %u field%s in %u group%s.%s%s\n",
                                     leaf_of(scope_path), b.fields, b.fields == 1 ? "" : "s",
                                     b.groups, b.groups == 1 ? "" : "s",
                                     b.notes->len > 0 ? "\n" : "", b.notes->str);
    status_text(summary);
    g_free(summary);
    g_string_free(b.notes, TRUE);
    return TRUE;
}

#include "event_sequence.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>

#include "fstapi.h"

G_DEFINE_QUARK(event-sequence-error-quark, event_sequence_error)

static guint path_depth(const gchar *path)
{
    guint depth = 0;
    for (const gchar *p = path; *p != '\0'; p++) {
        if (*p == '.') {
            depth++;
        }
    }
    return depth;
}

// One change per net: `paths` (gchar*) are all the names of the one VCD id
// code / FST handle. The deepest-scoped name becomes the primary (that's the
// form the netlist can resolve -- testbench-level aliases of a DUT port
// often can't be), the rest are kept as aliases.
static EventSeqChange *change_new(GPtrArray *paths, const gchar *value, guint seq)
{
    guint best = 0;
    for (guint i = 1; i < paths->len; i++) {
        if (path_depth(g_ptr_array_index(paths, i)) > path_depth(g_ptr_array_index(paths, best))) {
            best = i;
        }
    }

    EventSeqChange *c = g_new0(EventSeqChange, 1);
    c->path = g_strdup(g_ptr_array_index(paths, best));
    c->aliases = g_ptr_array_new_with_free_func(g_free);
    for (guint i = 0; i < paths->len; i++) {
        if (i != best) {
            g_ptr_array_add(c->aliases, g_strdup(g_ptr_array_index(paths, i)));
        }
    }
    c->value = g_strdup(value);
    c->seq = seq;
    c->level = -1;
    c->causes = g_ptr_array_new();
    return c;
}

static void change_free(gpointer data)
{
    EventSeqChange *c = data;
    g_free(c->path);
    g_ptr_array_unref(c->aliases);
    g_free(c->value);
    g_ptr_array_unref(c->causes);
    g_free(c);
}

static EventSeqResult *result_new(gint64 time, gboolean order_known)
{
    EventSeqResult *r = g_new0(EventSeqResult, 1);
    r->time = time;
    r->order_known = order_known;
    r->changes = g_ptr_array_new_with_free_func(change_free);
    return r;
}

void event_sequence_result_free(EventSeqResult *result)
{
    if (result == NULL) {
        return;
    }
    g_ptr_array_unref(result->changes);
    g_free(result->warning);
    g_free(result);
}

// Drops a trailing Verilog bit-range ("[63:0]", "[3]") so the path matches
// what the netlist side reports -- FST names often carry it, VCD $var lines
// carry it as a separate token we never see.
gchar *event_sequence_strip_range(const gchar *name)
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

static gchar *join_path(GPtrArray *scopes, const gchar *leaf)
{
    GString *s = g_string_new(NULL);
    for (guint i = 0; i < scopes->len; i++) {
        g_string_append(s, g_ptr_array_index(scopes, i));
        g_string_append_c(s, '.');
    }
    gchar *clean = event_sequence_strip_range(leaf);
    g_string_append(s, clean);
    g_free(clean);
    return g_string_free(s, FALSE);
}

/* ---------------------------------------------------------------- VCD -- */

typedef struct
{
    FILE *fp;
    gboolean is_pipe;
} DumpStream;

static gboolean stream_open(DumpStream *st, const gchar *path, gboolean gz, GError **error)
{
    st->is_pipe = gz;
    if (gz) {
        gchar *quoted = g_shell_quote(path);
        gchar *cmd = g_strdup_printf("gzip -dc %s", quoted);
        st->fp = popen(cmd, "r");
        g_free(cmd);
        g_free(quoted);
    } else {
        st->fp = fopen(path, "rb");
    }
    if (st->fp == NULL) {
        g_set_error(error,
                    EVENT_SEQUENCE_ERROR,
                    EVENT_SEQUENCE_ERROR_IO,
                    "Could not open %s: %s",
                    path,
                    g_strerror(errno));
        return FALSE;
    }
    return TRUE;
}

static void stream_close(DumpStream *st)
{
    if (st->fp != NULL) {
        if (st->is_pipe) {
            pclose(st->fp);
        } else {
            fclose(st->fp);
        }
        st->fp = NULL;
    }
}

// Reads one line (any length) into *buf, stripping the line terminator.
// Returns FALSE at EOF.
static gboolean read_line(FILE *fp, gchar **buf, gsize *cap)
{
    gsize len = 0;
    for (;;) {
        if (len + 2 > *cap) {
            *cap = *cap ? *cap * 2 : 256;
            *buf = g_realloc(*buf, *cap);
        }
        if (fgets(*buf + len, (int)(*cap - len), fp) == NULL) {
            if (len == 0) {
                return FALSE;
            }
            break;
        }
        len += strlen(*buf + len);
        if (len > 0 && (*buf)[len - 1] == '\n') {
            break;
        }
    }
    while (len > 0 && ((*buf)[len - 1] == '\n' || (*buf)[len - 1] == '\r')) {
        (*buf)[--len] = '\0';
    }
    return TRUE;
}

// id code -> GPtrArray of gchar* paths (every alias of that id code).
static void add_id_path(GHashTable *ids, const gchar *id, gchar *path)
{
    GPtrArray *paths = g_hash_table_lookup(ids, id);
    if (paths == NULL) {
        paths = g_ptr_array_new_with_free_func(g_free);
        g_hash_table_insert(ids, g_strdup(id), paths);
    }
    g_ptr_array_add(paths, path);
}

static gboolean vcd_parse_header(FILE *fp, GHashTable *ids, GError **error)
{
    GPtrArray *scopes = g_ptr_array_new_with_free_func(g_free);
    gchar *line = NULL;
    gsize cap = 0;
    gboolean done = FALSE;
    // Tokens of the declaration being accumulated -- a $var/$scope may in
    // principle span lines, so collect until its terminating $end.
    GPtrArray *decl = g_ptr_array_new_with_free_func(g_free);
    gboolean in_decl = FALSE;

    while (!done && read_line(fp, &line, &cap)) {
        gchar **tokens = g_strsplit_set(line, " \t", -1);
        for (gchar **tok = tokens; *tok != NULL; tok++) {
            const gchar *t = *tok;
            if (*t == '\0') {
                continue;
            }
            if (!in_decl) {
                if (strcmp(t, "$enddefinitions") == 0) {
                    done = TRUE;
                    break;
                }
                if (t[0] == '$') {
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
                g_ptr_array_add(scopes, g_strdup(g_ptr_array_index(decl, 2)));
            } else if (strcmp(kind, "$upscope") == 0) {
                if (scopes->len > 0) {
                    g_ptr_array_set_size(scopes, scopes->len - 1);
                }
            } else if (strcmp(kind, "$var") == 0 && decl->len >= 5) {
                // $var <type> <size> <id> <name> [range]
                const gchar *id = g_ptr_array_index(decl, 3);
                const gchar *name = g_ptr_array_index(decl, 4);
                add_id_path(ids, id, join_path(scopes, name));
            }
        }
        g_strfreev(tokens);
    }

    g_free(line);
    g_ptr_array_unref(decl);
    g_ptr_array_unref(scopes);

    if (!done) {
        g_set_error_literal(error,
                            EVENT_SEQUENCE_ERROR,
                            EVENT_SEQUENCE_ERROR_PARSE,
                            "VCD header has no $enddefinitions");
        return FALSE;
    }
    return TRUE;
}

static EventSeqResult *scan_vcd(const gchar *path, gboolean gz, gint64 time, GError **error)
{
    DumpStream st;
    if (!stream_open(&st, path, gz, error)) {
        return NULL;
    }

    GHashTable *ids = g_hash_table_new_full(g_str_hash,
                                            g_str_equal,
                                            g_free,
                                            (GDestroyNotify)g_ptr_array_unref);
    if (!vcd_parse_header(st.fp, ids, error)) {
        g_hash_table_unref(ids);
        stream_close(&st);
        return NULL;
    }

    EventSeqResult *result = result_new(time, TRUE);
    gchar *line = NULL;
    gsize cap = 0;
    gint64 cur_time = -1;
    guint seq = 0;

    while (read_line(st.fp, &line, &cap)) {
        const gchar *p = line;
        if (*p == '#') {
            cur_time = (gint64)g_ascii_strtoull(p + 1, NULL, 10);
            if (cur_time > time) {
                break;
            }
            continue;
        }
        // Fast path: nothing before the requested timestamp is parsed.
        if (cur_time != time || *p == '\0' || *p == '$') {
            continue;
        }

        const gchar *value;
        const gchar *id;
        gchar *value_copy = NULL;
        switch (*p) {
        case 'b':
        case 'B':
        case 'r':
        case 'R':
        case 's':
        case 'S': {
            const gchar *sp = strchr(p, ' ');
            if (sp == NULL) {
                continue;
            }
            value_copy = g_strndup(p, (gsize)(sp - p));
            value = value_copy;
            id = sp + 1;
            while (*id == ' ') {
                id++;
            }
            break;
        }
        default:
            value_copy = g_strndup(p, 1);
            value = value_copy;
            id = p + 1;
            break;
        }

        GPtrArray *paths = g_hash_table_lookup(ids, id);
        if (paths != NULL) {
            seq++;
            g_ptr_array_add(result->changes, change_new(paths, value, seq));
        }
        g_free(value_copy);
    }

    g_free(line);
    g_hash_table_unref(ids);
    stream_close(&st);
    return result;
}

/* ---------------------------------------------------------------- FST -- */

typedef struct
{
    gint64 time;
    GHashTable *handles; // fstHandle (as pointer) -> GPtrArray of gchar* paths
    EventSeqResult *result;
} FstScan;

static void fst_emit(FstScan *scan, uint64_t time, fstHandle handle, const gchar *value)
{
    if ((gint64)time != scan->time) {
        return;
    }
    GPtrArray *paths = g_hash_table_lookup(scan->handles, GUINT_TO_POINTER(handle));
    if (paths == NULL) {
        return;
    }
    g_ptr_array_add(scan->result->changes, change_new(paths, value, 0));
}

static void fst_value_cb(void *data, uint64_t time, fstHandle handle, const unsigned char *value)
{
    // Fixed-width callback: `value` is a NUL-terminated string of bit
    // characters. Prefix "b" for multi-bit to match the VCD form.
    FstScan *scan = data;
    const gchar *v = (const gchar *)value;
    if (v[0] != '\0' && v[1] != '\0') {
        gchar *vec = g_strconcat("b", v, NULL);
        fst_emit(scan, time, handle, vec);
        g_free(vec);
    } else {
        fst_emit(scan, time, handle, v);
    }
}

static void fst_varlen_cb(void *data,
                          uint64_t time,
                          fstHandle handle,
                          const unsigned char *value,
                          uint32_t len)
{
    FstScan *scan = data;
    gchar *s = g_strndup((const gchar *)value, len);
    gchar *tagged = g_strconcat("s", s, NULL);
    fst_emit(scan, time, handle, tagged);
    g_free(tagged);
    g_free(s);
}

static EventSeqResult *scan_fst(const gchar *path, gint64 time, GError **error)
{
    fstReaderContext *ctx = fstReaderOpen(path);
    if (ctx == NULL) {
        g_set_error(error,
                    EVENT_SEQUENCE_ERROR,
                    EVENT_SEQUENCE_ERROR_IO,
                    "Could not open %s as an FST file",
                    path);
        return NULL;
    }

    FstScan scan = {
        .time = time,
        .handles = g_hash_table_new_full(g_direct_hash,
                                         g_direct_equal,
                                         NULL,
                                         (GDestroyNotify)g_ptr_array_unref),
        .result = result_new(time, FALSE),
    };

    GPtrArray *scopes = g_ptr_array_new_with_free_func(g_free);
    struct fstHier *h;
    while ((h = fstReaderIterateHier(ctx)) != NULL) {
        if (h->htyp == FST_HT_SCOPE) {
            g_ptr_array_add(scopes, g_strdup(h->u.scope.name));
        } else if (h->htyp == FST_HT_UPSCOPE) {
            if (scopes->len > 0) {
                g_ptr_array_set_size(scopes, scopes->len - 1);
            }
        } else if (h->htyp == FST_HT_VAR) {
            GPtrArray *paths =
                g_hash_table_lookup(scan.handles, GUINT_TO_POINTER(h->u.var.handle));
            if (paths == NULL) {
                paths = g_ptr_array_new_with_free_func(g_free);
                g_hash_table_insert(scan.handles, GUINT_TO_POINTER(h->u.var.handle), paths);
            }
            g_ptr_array_add(paths, join_path(scopes, h->u.var.name));
        }
    }
    g_ptr_array_unref(scopes);

    fstReaderSetLimitTimeRange(ctx, (uint64_t)time, (uint64_t)time);
    fstReaderSetFacProcessMaskAll(ctx);
    fstReaderIterBlocks2(ctx, fst_value_cb, fst_varlen_cb, &scan, NULL);
    fstReaderClose(ctx);

    g_hash_table_unref(scan.handles);
    return scan.result;
}

/* ------------------------------------------------------------- public -- */

EventSeqResult *event_sequence_scan(const gchar *dump_path, gint64 time, GError **error)
{
    if (dump_path == NULL || strcmp(dump_path, "-vcd") == 0) {
        g_set_error_literal(error,
                            EVENT_SEQUENCE_ERROR,
                            EVENT_SEQUENCE_ERROR_UNSUPPORTED,
                            "The dump was read from stdin, so there is no file to re-scan.");
        return NULL;
    }

    unsigned char magic[4] = {0};
    FILE *fp = fopen(dump_path, "rb");
    if (fp == NULL) {
        g_set_error(error,
                    EVENT_SEQUENCE_ERROR,
                    EVENT_SEQUENCE_ERROR_IO,
                    "Could not open %s: %s",
                    dump_path,
                    g_strerror(errno));
        return NULL;
    }
    size_t n = fread(magic, 1, sizeof(magic), fp);
    fclose(fp);

    if (n >= 2 && magic[0] == 0x1f && magic[1] == 0x8b) {
        return scan_vcd(dump_path, TRUE, time, error);
    }
    // FST's first block is the header block, type byte 0; text VCD never
    // starts with a NUL.
    if (n >= 1 && magic[0] == 0) {
        return scan_fst(dump_path, time, error);
    }
    if (n >= 3 && memcmp(magic, "BZh", 3) == 0) {
        g_set_error_literal(error,
                            EVENT_SEQUENCE_ERROR,
                            EVENT_SEQUENCE_ERROR_UNSUPPORTED,
                            "bzip2-compressed dumps aren't supported by Event Sequence.");
        return NULL;
    }
    if (n >= 4 && memcmp(magic, "GHDL", 4) == 0) {
        g_set_error_literal(error,
                            EVENT_SEQUENCE_ERROR,
                            EVENT_SEQUENCE_ERROR_UNSUPPORTED,
                            "GHW dumps aren't supported by Event Sequence.");
        return NULL;
    }
    return scan_vcd(dump_path, FALSE, time, error);
}

/* ---------------------------------------------------------- causality -- */

typedef struct
{
    GPtrArray *changes;
    // Per-change state for the memoized level walk: 0 = unvisited,
    // 1 = on the current DFS stack, 2 = done.
    guint8 *state;
    GHashTable *index; // path -> GUINT_TO_POINTER(index + 1)
} LevelCtx;

static gint compute_level(LevelCtx *ctx, guint i)
{
    EventSeqChange *c = g_ptr_array_index(ctx->changes, i);
    if (ctx->state[i] == 2) {
        return c->level;
    }
    if (ctx->state[i] == 1) {
        return -1; // cycle: ignore this edge
    }
    ctx->state[i] = 1;

    gint level = 0;
    for (guint k = 0; k < c->causes->len; k++) {
        EventSeqChange *cause = g_ptr_array_index(c->causes, k);
        gint cl = compute_level(ctx, cause->index);
        if (cl >= 0 && cl + 1 > level) {
            level = cl + 1;
        }
    }
    c->level = level;
    ctx->state[i] = 2;
    return level;
}

void event_sequence_assign_causality(EventSeqResult *result,
                                     EventSeqFaninFunc fanin,
                                     gpointer user_data,
                                     guint max_queries)
{
    GPtrArray *changes = result->changes;
    GHashTable *index = g_hash_table_new(g_str_hash, g_str_equal);
    for (guint i = 0; i < changes->len; i++) {
        EventSeqChange *c = g_ptr_array_index(changes, i);
        c->level = -1;
        c->index = i;
        g_ptr_array_set_size(c->causes, 0);
        c->dumped_before_cause = FALSE;
        if (!g_hash_table_contains(index, c->path)) {
            g_hash_table_insert(index, c->path, GUINT_TO_POINTER(i + 1));
        }
        for (guint k = 0; k < c->aliases->len; k++) {
            gchar *alias = g_ptr_array_index(c->aliases, k);
            if (!g_hash_table_contains(index, alias)) {
                g_hash_table_insert(index, alias, GUINT_TO_POINTER(i + 1));
            }
        }
    }

    guint queried = 0;
    gboolean capped = FALSE;
    gboolean any_unknown = FALSE;
    // Signals whose fan-in is known; only these get a level. A signal
    // whose lookup failed stays at -1 rather than a misleading 0.
    guint8 *known = g_new0(guint8, changes->len);

    for (guint i = 0; i < changes->len; i++) {
        EventSeqChange *c = g_ptr_array_index(changes, i);
        if (max_queries != 0 && queried >= max_queries) {
            capped = TRUE;
            break;
        }
        queried++;
        GPtrArray *cone = fanin(c->path, user_data);
        for (guint k = 0; cone == NULL && k < c->aliases->len; k++) {
            cone = fanin(g_ptr_array_index(c->aliases, k), user_data);
        }
        if (cone == NULL) {
            any_unknown = TRUE;
            continue;
        }
        known[i] = 1;
        for (guint k = 0; k < cone->len; k++) {
            guint ci = GPOINTER_TO_UINT(g_hash_table_lookup(index, g_ptr_array_index(cone, k)));
            if (ci == 0 || ci - 1 == i) {
                continue;
            }
            EventSeqChange *cause = g_ptr_array_index(changes, ci - 1);
            if (!g_ptr_array_find(c->causes, cause, NULL)) {
                g_ptr_array_add(c->causes, cause);
            }
        }
        g_ptr_array_unref(cone);
    }

    LevelCtx ctx = {
        .changes = changes,
        .state = g_new0(guint8, changes->len),
        .index = index,
    };
    for (guint i = 0; i < changes->len; i++) {
        if (known[i]) {
            compute_level(&ctx, i);
        }
    }
    // A cause whose own fan-in was never looked up has an unknown level;
    // compute_level() above treated it as 0 via its default, which is only
    // a lower bound -- fine for ordering, and the signal itself is shown
    // as "?" because known[] is false.
    for (guint i = 0; i < changes->len; i++) {
        EventSeqChange *c = g_ptr_array_index(changes, i);
        if (!known[i]) {
            c->level = -1;
        }
        if (result->order_known) {
            for (guint k = 0; k < c->causes->len; k++) {
                EventSeqChange *cause = g_ptr_array_index(c->causes, k);
                if (cause->seq > c->seq) {
                    c->dumped_before_cause = TRUE;
                }
            }
        }
    }

    g_free(ctx.state);
    g_free(known);
    g_hash_table_unref(index);

    result->causality_known = TRUE;
    g_free(result->warning);
    result->warning = NULL;
    if (capped) {
        result->warning = g_strdup_printf("Causality computed for the first %u signals only "
                                          "(the rest are shown unranked).",
                                          max_queries);
    } else if (any_unknown) {
        result->warning = g_strdup("Some signals aren't in the netlist (or couldn't be "
                                   "queried) and are shown unranked.");
    }
}

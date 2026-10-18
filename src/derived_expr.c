#include "derived_expr.h"

#include <ctype.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

G_DEFINE_QUARK(derived-expr-error-quark, derived_expr_error)

#define MAX_ARITH_WIDTH 64
#define MAX_WIDTH 65536

typedef enum
{
    K_LIT,
    K_REF,
    K_UNARY,
    K_BINARY,
    K_TERNARY,
    K_CONCAT,
    K_REPL,
    K_BITSEL,
} Kind;

typedef enum
{
    OP_NOT,
    OP_LNOT,
    OP_NEG,
    OP_RAND,
    OP_ROR,
    OP_RXOR,
    OP_RNAND,
    OP_RNOR,
    OP_RXNOR,
    OP_ADD,
    OP_SUB,
    OP_MUL,
    OP_DIV,
    OP_MOD,
    OP_SHL,
    OP_SHR,
    OP_LT,
    OP_LE,
    OP_GT,
    OP_GE,
    OP_EQ,
    OP_NE,
    OP_AND,
    OP_OR,
    OP_XOR,
    OP_XNOR,
    OP_LAND,
    OP_LOR,
} Op;

typedef struct DNode DNode;

struct DNode
{
    Kind kind;
    Op op;
    DNode *a, *b, *c;
    GPtrArray *items; // K_CONCAT operands, MSB first
    gchar *name; // K_REF: canonical signal name
    guint count; // K_REPL
    guint msb, lsb; // K_BITSEL (a bit-select has msb == lsb)
    guint lit_width; // K_LIT
    guint8 *lit_bits; // K_LIT, MSB first

    // Filled in per evaluation.
    guint width;
    guint8 *buf; // current value, MSB first
    gint slot; // K_REF: index into the inputs array
};

struct _DerivedExpr
{
    DNode *root;
    GPtrArray *refs; // distinct canonical names, first-appearance order
};

static void dnode_free(DNode *n)
{
    if (n == NULL) {
        return;
    }
    dnode_free(n->a);
    dnode_free(n->b);
    dnode_free(n->c);
    if (n->items != NULL) {
        for (guint i = 0; i < n->items->len; i++) {
            dnode_free(g_ptr_array_index(n->items, i));
        }
        g_ptr_array_unref(n->items);
    }
    g_free(n->name);
    g_free(n->lit_bits);
    g_free(n->buf);
    g_free(n);
}

void derived_expr_free(DerivedExpr *expr)
{
    if (expr == NULL) {
        return;
    }
    dnode_free(expr->root);
    if (expr->refs != NULL) {
        g_ptr_array_unref(expr->refs);
    }
    g_free(expr);
}

GPtrArray *derived_expr_references(const DerivedExpr *expr)
{
    GPtrArray *out = g_ptr_array_new();
    for (guint i = 0; i < expr->refs->len; i++) {
        g_ptr_array_add(out, g_ptr_array_index(expr->refs, i));
    }
    return out;
}

/* ------------------------------------------------------------ parser -- */

typedef struct
{
    const gchar *s;
    gsize pos;
    DerivedNameResolver resolve;
    gpointer user_data;
    GPtrArray *refs;
    GError **error;
    gboolean failed;
} Parser;

static void fail(Parser *p, DerivedExprError code, const gchar *fmt, ...) G_GNUC_PRINTF(3, 4);

static void fail(Parser *p, DerivedExprError code, const gchar *fmt, ...)
{
    if (p->failed) {
        return;
    }
    p->failed = TRUE;
    va_list ap;
    va_start(ap, fmt);
    gchar *msg = g_strdup_vprintf(fmt, ap);
    va_end(ap);
    g_set_error(p->error, DERIVED_EXPR_ERROR, code, "%s (at column %" G_GSIZE_FORMAT ")", msg,
                p->pos + 1);
    g_free(msg);
}

static void skip_ws(Parser *p)
{
    while (p->s[p->pos] != '\0' && isspace((guchar)p->s[p->pos])) {
        p->pos++;
    }
}

static gboolean peek_is(Parser *p, const gchar *tok)
{
    skip_ws(p);
    return strncmp(p->s + p->pos, tok, strlen(tok)) == 0;
}

static gboolean accept(Parser *p, const gchar *tok)
{
    if (peek_is(p, tok)) {
        p->pos += strlen(tok);
        return TRUE;
    }
    return FALSE;
}

static DNode *node_new(Kind kind)
{
    DNode *n = g_new0(DNode, 1);
    n->kind = kind;
    n->slot = -1;
    return n;
}

static DNode *make_binary(Op op, DNode *a, DNode *b)
{
    DNode *n = node_new(K_BINARY);
    n->op = op;
    n->a = a;
    n->b = b;
    return n;
}

static DNode *parse_expr(Parser *p);

static guint bits_needed(guint64 v)
{
    guint w = 1;
    while (w < 64 && (v >> w) != 0) {
        w++;
    }
    return w;
}

static void set_lit_u64(DNode *n, guint width, guint64 v)
{
    n->kind = K_LIT;
    n->lit_width = width;
    n->lit_bits = g_new0(guint8, width);
    for (guint i = 0; i < width; i++) {
        guint k = width - 1 - i; // bit number from LSB
        n->lit_bits[i] = (k < 64 && ((v >> k) & 1)) ? DERIVED_BIT_1 : DERIVED_BIT_0;
    }
}

// Number: `42`, `8'hFF`, `4'b10x1`, `'d5`.
static DNode *parse_number(Parser *p)
{
    gsize start = p->pos;
    const gchar *s = p->s;

    guint64 size = 0;
    gboolean sized = FALSE;
    if (isdigit((guchar)s[p->pos])) {
        guint64 v = 0;
        while (isdigit((guchar)s[p->pos]) || s[p->pos] == '_') {
            if (s[p->pos] != '_') {
                guint64 nv = v * 10 + (guint64)(s[p->pos] - '0');
                if (nv < v || v > G_MAXUINT64 / 10) {
                    fail(p, DERIVED_EXPR_ERROR_UNSUPPORTED, "number too large (limit is 64 bits)");
                    return NULL;
                }
                v = nv;
            }
            p->pos++;
        }
        if (s[p->pos] != '\'') {
            DNode *n = node_new(K_LIT);
            set_lit_u64(n, bits_needed(v), v);
            return n;
        }
        size = v;
        sized = TRUE;
    }

    if (s[p->pos] != '\'') {
        fail(p, DERIVED_EXPR_ERROR_SYNTAX, "expected a number");
        return NULL;
    }
    p->pos++; // the tick
    gchar base = (gchar)tolower((guchar)s[p->pos]);
    if (base != 'b' && base != 'o' && base != 'd' && base != 'h') {
        p->pos = start;
        fail(p, DERIVED_EXPR_ERROR_SYNTAX, "expected a base (b, o, d or h) after the tick");
        return NULL;
    }
    p->pos++;

    gsize dstart = p->pos;
    while (isalnum((guchar)s[p->pos]) || s[p->pos] == '_' || s[p->pos] == '?') {
        p->pos++;
    }
    if (p->pos == dstart) {
        fail(p, DERIVED_EXPR_ERROR_SYNTAX, "expected digits after the base");
        return NULL;
    }

    if (sized && (size == 0 || size > MAX_WIDTH)) {
        fail(p, DERIVED_EXPR_ERROR_UNSUPPORTED, "literal width %" G_GUINT64_FORMAT " is out of range",
             size);
        return NULL;
    }

    GArray *bits = g_array_new(FALSE, FALSE, sizeof(guint8)); // MSB first
    gboolean ok = TRUE;

    if (base == 'd') {
        guint64 v = 0;
        for (gsize i = dstart; i < p->pos; i++) {
            if (s[i] == '_') {
                continue;
            }
            if (!isdigit((guchar)s[i])) {
                p->pos = i;
                fail(p, DERIVED_EXPR_ERROR_SYNTAX, "x/z digits aren't allowed in decimal literals");
                ok = FALSE;
                break;
            }
            if (v > (G_MAXUINT64 - (guint64)(s[i] - '0')) / 10) {
                fail(p, DERIVED_EXPR_ERROR_UNSUPPORTED, "number too large (limit is 64 bits)");
                ok = FALSE;
                break;
            }
            v = v * 10 + (guint64)(s[i] - '0');
        }
        if (ok) {
            guint w = sized ? (guint)size : bits_needed(v);
            for (guint i = 0; i < w; i++) {
                guint k = w - 1 - i;
                guint8 b = (k < 64 && ((v >> k) & 1)) ? DERIVED_BIT_1 : DERIVED_BIT_0;
                g_array_append_val(bits, b);
            }
        }
    } else {
        guint per = base == 'b' ? 1 : base == 'o' ? 3 : 4;
        for (gsize i = dstart; ok && i < p->pos; i++) {
            gchar c = (gchar)tolower((guchar)s[i]);
            if (c == '_') {
                continue;
            }
            gint digit = -1;
            guint8 special = 0;
            if (c == 'x' || c == '?' ) {
                special = c == '?' ? DERIVED_BIT_Z : DERIVED_BIT_X;
            } else if (c == 'z') {
                special = DERIVED_BIT_Z;
            } else if (isdigit((guchar)c)) {
                digit = c - '0';
            } else if (c >= 'a' && c <= 'f') {
                digit = 10 + (c - 'a');
            } else {
                p->pos = i;
                fail(p, DERIVED_EXPR_ERROR_SYNTAX, "bad digit '%c' in literal", s[i]);
                ok = FALSE;
                break;
            }
            if (digit >= (1 << per)) {
                p->pos = i;
                fail(p, DERIVED_EXPR_ERROR_SYNTAX, "digit '%c' is out of range for base %c", s[i],
                     base);
                ok = FALSE;
                break;
            }
            for (guint b = 0; b < per; b++) {
                guint k = per - 1 - b;
                guint8 bit = digit >= 0 ? (((digit >> k) & 1) ? DERIVED_BIT_1 : DERIVED_BIT_0)
                                        : special;
                g_array_append_val(bits, bit);
            }
        }
        if (ok && sized) {
            // Fit to the declared width: drop high zeros / pad with 0
            // (or the leading x/z, per Verilog).
            guint w = (guint)size;
            if (bits->len > w) {
                g_array_remove_range(bits, 0, bits->len - w);
            } else if (bits->len < w) {
                guint8 pad = g_array_index(bits, guint8, 0);
                if (pad != DERIVED_BIT_X && pad != DERIVED_BIT_Z) {
                    pad = DERIVED_BIT_0;
                }
                guint missing = w - bits->len;
                for (guint i = 0; i < missing; i++) {
                    g_array_prepend_val(bits, pad);
                }
            }
        } else if (ok) {
            // Unsized: trim leading known zeros down to the value's width.
            while (bits->len > 1 && g_array_index(bits, guint8, 0) == DERIVED_BIT_0) {
                g_array_remove_index(bits, 0);
            }
        }
    }

    DNode *n = NULL;
    if (ok && bits->len > 0) {
        n = node_new(K_LIT);
        n->lit_width = bits->len;
        n->lit_bits = g_memdup2(bits->data, bits->len);
    } else if (ok) {
        fail(p, DERIVED_EXPR_ERROR_SYNTAX, "empty literal");
    }
    g_array_free(bits, TRUE);
    return n;
}

static gboolean bare_name_char(gchar c, gboolean first)
{
    if (isalpha((guchar)c) || c == '_') {
        return TRUE;
    }
    return !first && (isdigit((guchar)c) || c == '.' || c == '$');
}

static gchar *resolve_name(Parser *p, const gchar *name)
{
    if (p->resolve == NULL) {
        return g_strdup(name);
    }
    return p->resolve(name, p->user_data);
}

static void note_ref(Parser *p, const gchar *canonical)
{
    for (guint i = 0; i < p->refs->len; i++) {
        if (g_str_equal(g_ptr_array_index(p->refs, i), canonical)) {
            return;
        }
    }
    g_ptr_array_add(p->refs, g_strdup(canonical));
}

static DNode *make_ref(Parser *p, gchar *canonical)
{
    DNode *n = node_new(K_REF);
    n->name = canonical;
    note_ref(p, canonical);
    return n;
}

static DNode *parse_name(Parser *p)
{
    const gchar *s = p->s;

    if (s[p->pos] == '"') {
        gsize start = ++p->pos;
        GString *name = g_string_new(NULL);
        while (s[p->pos] != '\0' && s[p->pos] != '"') {
            if (s[p->pos] == '\\' && s[p->pos + 1] != '\0') {
                p->pos++;
            }
            g_string_append_c(name, s[p->pos]);
            p->pos++;
        }
        if (s[p->pos] != '"') {
            p->pos = start;
            fail(p, DERIVED_EXPR_ERROR_SYNTAX, "unterminated quoted signal name");
            g_string_free(name, TRUE);
            return NULL;
        }
        p->pos++;
        gchar *canonical = resolve_name(p, name->str);
        if (canonical == NULL) {
            fail(p, DERIVED_EXPR_ERROR_UNKNOWN_SIGNAL, "no signal named \"%s\"", name->str);
            g_string_free(name, TRUE);
            return NULL;
        }
        g_string_free(name, TRUE);
        return make_ref(p, canonical);
    }

    gsize start = p->pos;
    if (!bare_name_char(s[p->pos], TRUE)) {
        fail(p, DERIVED_EXPR_ERROR_SYNTAX, "unexpected '%c'", s[p->pos] ? s[p->pos] : ' ');
        return NULL;
    }
    while (bare_name_char(s[p->pos], p->pos == start ? TRUE : FALSE)) {
        p->pos++;
    }
    gchar *base = g_strndup(s + start, p->pos - start);

    // `count[7:0]` may be one declared signal name, or `count` followed by a
    // select. Prefer the declared name if it exists (without a resolver
    // there's no way to know, and every name "exists": always a select).
    if (p->resolve != NULL && s[p->pos] == '[') {
        const gchar *close = strchr(s + p->pos, ']');
        if (close != NULL) {
            // Declared names never contain spaces, so ignore any in the text.
            GString *compact = g_string_new(base);
            for (const gchar *c = s + p->pos; c <= close; c++) {
                if (!isspace((guchar)*c)) {
                    g_string_append_c(compact, *c);
                }
            }
            gchar *canonical = resolve_name(p, compact->str);
            g_string_free(compact, TRUE);
            if (canonical != NULL) {
                p->pos = (gsize)(close - s) + 1;
                g_free(base);
                return make_ref(p, canonical);
            }
        }
    }

    gchar *canonical = resolve_name(p, base);
    if (canonical == NULL) {
        p->pos = start;
        fail(p, DERIVED_EXPR_ERROR_UNKNOWN_SIGNAL, "no signal named '%s'", base);
        g_free(base);
        return NULL;
    }
    g_free(base);
    return make_ref(p, canonical);
}

static gboolean parse_uint(Parser *p, guint *out)
{
    skip_ws(p);
    if (!isdigit((guchar)p->s[p->pos])) {
        fail(p, DERIVED_EXPR_ERROR_SYNTAX, "expected a constant number");
        return FALSE;
    }
    guint64 v = 0;
    while (isdigit((guchar)p->s[p->pos])) {
        v = v * 10 + (guint64)(p->s[p->pos] - '0');
        if (v > MAX_WIDTH) {
            fail(p, DERIVED_EXPR_ERROR_UNSUPPORTED, "index or count is too large");
            return FALSE;
        }
        p->pos++;
    }
    *out = (guint)v;
    return TRUE;
}

static DNode *parse_primary(Parser *p)
{
    skip_ws(p);
    const gchar *s = p->s;

    if (s[p->pos] == '\0') {
        fail(p, DERIVED_EXPR_ERROR_SYNTAX, "unexpected end of expression");
        return NULL;
    }
    if (s[p->pos] == '(') {
        p->pos++;
        DNode *inner = parse_expr(p);
        if (inner == NULL) {
            return NULL;
        }
        if (!accept(p, ")")) {
            fail(p, DERIVED_EXPR_ERROR_SYNTAX, "expected ')'");
            dnode_free(inner);
            return NULL;
        }
        return inner;
    }
    if (s[p->pos] == '{') {
        p->pos++;
        skip_ws(p);

        // Replication `{N{expr}}`: a constant, then '{'.
        if (isdigit((guchar)s[p->pos])) {
            gsize save = p->pos;
            guint count = 0;
            if (parse_uint(p, &count) && peek_is(p, "{")) {
                p->pos++;
                // `{N{a, b}}`: the inner list is itself a concatenation.
                GPtrArray *body_items = g_ptr_array_new();
                gboolean body_ok = TRUE;
                for (;;) {
                    DNode *item = parse_expr(p);
                    if (item == NULL) {
                        body_ok = FALSE;
                        break;
                    }
                    g_ptr_array_add(body_items, item);
                    if (accept(p, ",")) {
                        continue;
                    }
                    break;
                }
                if (body_ok && (!accept(p, "}") || !accept(p, "}"))) {
                    fail(p, DERIVED_EXPR_ERROR_SYNTAX, "expected '}}' to close the replication");
                    body_ok = FALSE;
                }
                if (body_ok && count == 0) {
                    fail(p, DERIVED_EXPR_ERROR_SYNTAX, "replication count must be at least 1");
                    body_ok = FALSE;
                }
                if (!body_ok) {
                    for (guint i = 0; i < body_items->len; i++) {
                        dnode_free(g_ptr_array_index(body_items, i));
                    }
                    g_ptr_array_unref(body_items);
                    return NULL;
                }

                DNode *body;
                if (body_items->len == 1) {
                    body = g_ptr_array_index(body_items, 0);
                    g_ptr_array_unref(body_items);
                } else {
                    body = node_new(K_CONCAT);
                    body->items = body_items;
                }
                DNode *n = node_new(K_REPL);
                n->count = count;
                n->a = body;
                return n;
            }
            if (p->failed) {
                return NULL;
            }
            p->pos = save; // an ordinary concatenation starting with a number
        }

        DNode *n = node_new(K_CONCAT);
        n->items = g_ptr_array_new();
        for (;;) {
            DNode *item = parse_expr(p);
            if (item == NULL) {
                dnode_free(n);
                return NULL;
            }
            g_ptr_array_add(n->items, item);
            if (accept(p, ",")) {
                continue;
            }
            if (accept(p, "}")) {
                break;
            }
            fail(p, DERIVED_EXPR_ERROR_SYNTAX, "expected ',' or '}' in the concatenation");
            dnode_free(n);
            return NULL;
        }
        return n;
    }
    if (isdigit((guchar)s[p->pos]) || s[p->pos] == '\'') {
        return parse_number(p);
    }
    return parse_name(p);
}

static DNode *parse_postfix(Parser *p)
{
    DNode *n = parse_primary(p);
    while (n != NULL && peek_is(p, "[")) {
        p->pos++;
        guint hi = 0, lo = 0;
        if (!parse_uint(p, &hi)) {
            dnode_free(n);
            return NULL;
        }
        lo = hi;
        if (accept(p, ":")) {
            if (!parse_uint(p, &lo)) {
                dnode_free(n);
                return NULL;
            }
        }
        if (!accept(p, "]")) {
            fail(p, DERIVED_EXPR_ERROR_SYNTAX,
                 "expected ']' (bit and part select indexes must be constants)");
            dnode_free(n);
            return NULL;
        }
        if (lo > hi) {
            fail(p, DERIVED_EXPR_ERROR_SYNTAX, "part select [%u:%u] has its bounds reversed", hi, lo);
            dnode_free(n);
            return NULL;
        }
        DNode *sel = node_new(K_BITSEL);
        sel->a = n;
        sel->msb = hi;
        sel->lsb = lo;
        n = sel;
    }
    return n;
}

static DNode *parse_unary(Parser *p)
{
    skip_ws(p);
    struct
    {
        const gchar *tok;
        Op op;
    } ops[] = {
        // Longest tokens first: `~&` must not be read as `~` then `&`.
        {"~&", OP_RNAND}, {"~|", OP_RNOR}, {"~^", OP_RXNOR}, {"^~", OP_RXNOR},
        {"~", OP_NOT},    {"!", OP_LNOT},  {"-", OP_NEG},    {"&", OP_RAND},
        {"|", OP_ROR},    {"^", OP_RXOR},
    };
    for (guint i = 0; i < G_N_ELEMENTS(ops); i++) {
        if (peek_is(p, ops[i].tok)) {
            p->pos += strlen(ops[i].tok);
            DNode *operand = parse_unary(p);
            if (operand == NULL) {
                return NULL;
            }
            DNode *n = node_new(K_UNARY);
            n->op = ops[i].op;
            n->a = operand;
            return n;
        }
    }
    if (peek_is(p, "+")) { // unary plus: a no-op
        p->pos++;
        return parse_unary(p);
    }
    return parse_postfix(p);
}

typedef DNode *(*SubParser)(Parser *p);

typedef struct
{
    const gchar *tok;
    Op op;
} BinTok;

// One left-associative binary level. A token only matches if it isn't the
// start of a longer operator at the same position (e.g. `|` vs `||`,
// `<` vs `<<` / `<=`), which `reject` lists.
static DNode *parse_level(Parser *p, SubParser next, const BinTok *toks, guint n_toks,
                          const gchar *const *reject)
{
    DNode *left = next(p);
    while (left != NULL) {
        skip_ws(p);
        gboolean matched = FALSE;
        for (guint i = 0; i < n_toks && !matched; i++) {
            if (!peek_is(p, toks[i].tok)) {
                continue;
            }
            gboolean blocked = FALSE;
            for (guint r = 0; reject != NULL && reject[r] != NULL; r++) {
                if (peek_is(p, reject[r]) && strlen(reject[r]) > strlen(toks[i].tok)) {
                    blocked = TRUE;
                }
            }
            if (blocked) {
                continue;
            }
            p->pos += strlen(toks[i].tok);
            DNode *right = next(p);
            if (right == NULL) {
                dnode_free(left);
                return NULL;
            }
            left = make_binary(toks[i].op, left, right);
            matched = TRUE;
        }
        if (!matched) {
            break;
        }
    }
    return left;
}

static DNode *parse_mul(Parser *p)
{
    static const BinTok t[] = {{"*", OP_MUL}, {"/", OP_DIV}, {"%", OP_MOD}};
    return parse_level(p, parse_unary, t, G_N_ELEMENTS(t), NULL);
}

static DNode *parse_add(Parser *p)
{
    static const BinTok t[] = {{"+", OP_ADD}, {"-", OP_SUB}};
    return parse_level(p, parse_mul, t, G_N_ELEMENTS(t), NULL);
}

static DNode *parse_shift(Parser *p)
{
    static const BinTok t[] = {{"<<", OP_SHL}, {">>", OP_SHR}};
    return parse_level(p, parse_add, t, G_N_ELEMENTS(t), NULL);
}

static DNode *parse_rel(Parser *p)
{
    static const BinTok t[] = {{"<=", OP_LE}, {">=", OP_GE}, {"<", OP_LT}, {">", OP_GT}};
    static const gchar *const reject[] = {"<<", ">>", NULL};
    return parse_level(p, parse_shift, t, G_N_ELEMENTS(t), reject);
}

static DNode *parse_eq(Parser *p)
{
    static const BinTok t[] = {{"==", OP_EQ}, {"!=", OP_NE}};
    return parse_level(p, parse_rel, t, G_N_ELEMENTS(t), NULL);
}

static DNode *parse_band(Parser *p)
{
    static const BinTok t[] = {{"&", OP_AND}};
    static const gchar *const reject[] = {"&&", NULL};
    return parse_level(p, parse_eq, t, G_N_ELEMENTS(t), reject);
}

static DNode *parse_bxor(Parser *p)
{
    static const BinTok t[] = {{"^~", OP_XNOR}, {"~^", OP_XNOR}, {"^", OP_XOR}};
    return parse_level(p, parse_band, t, G_N_ELEMENTS(t), NULL);
}

static DNode *parse_bor(Parser *p)
{
    static const BinTok t[] = {{"|", OP_OR}};
    static const gchar *const reject[] = {"||", NULL};
    return parse_level(p, parse_bxor, t, G_N_ELEMENTS(t), reject);
}

static DNode *parse_land(Parser *p)
{
    static const BinTok t[] = {{"&&", OP_LAND}};
    return parse_level(p, parse_bor, t, G_N_ELEMENTS(t), NULL);
}

static DNode *parse_lor(Parser *p)
{
    static const BinTok t[] = {{"||", OP_LOR}};
    return parse_level(p, parse_land, t, G_N_ELEMENTS(t), NULL);
}

static DNode *parse_ternary(Parser *p)
{
    DNode *cond = parse_lor(p);
    if (cond == NULL) {
        return NULL;
    }
    if (!accept(p, "?")) {
        return cond;
    }
    DNode *yes = parse_expr(p);
    if (yes == NULL) {
        dnode_free(cond);
        return NULL;
    }
    if (!accept(p, ":")) {
        fail(p, DERIVED_EXPR_ERROR_SYNTAX, "expected ':' in the ?: expression");
        dnode_free(cond);
        dnode_free(yes);
        return NULL;
    }
    DNode *no = parse_expr(p);
    if (no == NULL) {
        dnode_free(cond);
        dnode_free(yes);
        return NULL;
    }
    DNode *n = node_new(K_TERNARY);
    n->a = cond;
    n->b = yes;
    n->c = no;
    return n;
}

static DNode *parse_expr(Parser *p)
{
    return parse_ternary(p);
}

DerivedExpr *derived_expr_parse(const gchar *text,
                                DerivedNameResolver resolve,
                                gpointer user_data,
                                GError **error)
{
    Parser p = {
        .s = text,
        .resolve = resolve,
        .user_data = user_data,
        .refs = g_ptr_array_new_with_free_func(g_free),
        .error = error,
    };

    DNode *root = parse_expr(&p);
    if (root != NULL) {
        skip_ws(&p);
        if (p.s[p.pos] != '\0') {
            fail(&p, DERIVED_EXPR_ERROR_SYNTAX, "unexpected '%c'", p.s[p.pos]);
            dnode_free(root);
            root = NULL;
        }
    }
    if (root == NULL) {
        if (!p.failed) {
            fail(&p, DERIVED_EXPR_ERROR_SYNTAX, "empty expression");
        }
        g_ptr_array_unref(p.refs);
        return NULL;
    }

    DerivedExpr *expr = g_new0(DerivedExpr, 1);
    expr->root = root;
    expr->refs = p.refs;
    return expr;
}

/* ----------------------------------------------------------- to_string -- */

static const gchar *op_text(Op op)
{
    switch (op) {
        case OP_NOT: return "~";
        case OP_LNOT: return "!";
        case OP_NEG: return "-";
        case OP_RAND: return "&";
        case OP_ROR: return "|";
        case OP_RXOR: return "^";
        case OP_RNAND: return "~&";
        case OP_RNOR: return "~|";
        case OP_RXNOR: return "~^";
        case OP_ADD: return "+";
        case OP_SUB: return "-";
        case OP_MUL: return "*";
        case OP_DIV: return "/";
        case OP_MOD: return "%";
        case OP_SHL: return "<<";
        case OP_SHR: return ">>";
        case OP_LT: return "<";
        case OP_LE: return "<=";
        case OP_GT: return ">";
        case OP_GE: return ">=";
        case OP_EQ: return "==";
        case OP_NE: return "!=";
        case OP_AND: return "&";
        case OP_OR: return "|";
        case OP_XOR: return "^";
        case OP_XNOR: return "~^";
        case OP_LAND: return "&&";
        case OP_LOR: return "||";
        default: return "?";
    }
}

static gboolean is_plain_identifier(const gchar *name)
{
    if (!bare_name_char(name[0], TRUE)) {
        return FALSE;
    }
    for (const gchar *c = name + 1; *c != '\0'; c++) {
        if (!bare_name_char(*c, FALSE)) {
            return FALSE;
        }
    }
    return TRUE;
}

static void append_node(GString *out, const DNode *n)
{
    switch (n->kind) {
        case K_LIT:
            g_string_append_printf(out, "%u'b", n->lit_width);
            for (guint i = 0; i < n->lit_width; i++) {
                g_string_append_c(out, "01xz"[n->lit_bits[i]]);
            }
            break;
        case K_REF:
            if (is_plain_identifier(n->name)) {
                g_string_append(out, n->name);
            } else {
                g_string_append_c(out, '"');
                for (const gchar *c = n->name; *c != '\0'; c++) {
                    if (*c == '"' || *c == '\\') {
                        g_string_append_c(out, '\\');
                    }
                    g_string_append_c(out, *c);
                }
                g_string_append_c(out, '"');
            }
            break;
        case K_UNARY:
            g_string_append_c(out, '(');
            g_string_append(out, op_text(n->op));
            append_node(out, n->a);
            g_string_append_c(out, ')');
            break;
        case K_BINARY:
            g_string_append_c(out, '(');
            append_node(out, n->a);
            g_string_append_printf(out, " %s ", op_text(n->op));
            append_node(out, n->b);
            g_string_append_c(out, ')');
            break;
        case K_TERNARY:
            g_string_append_c(out, '(');
            append_node(out, n->a);
            g_string_append(out, " ? ");
            append_node(out, n->b);
            g_string_append(out, " : ");
            append_node(out, n->c);
            g_string_append_c(out, ')');
            break;
        case K_CONCAT:
            g_string_append_c(out, '{');
            for (guint i = 0; i < n->items->len; i++) {
                if (i > 0) {
                    g_string_append(out, ", ");
                }
                append_node(out, g_ptr_array_index(n->items, i));
            }
            g_string_append_c(out, '}');
            break;
        case K_REPL:
            g_string_append_printf(out, "{%u{", n->count);
            append_node(out, n->a);
            g_string_append(out, "}}");
            break;
        case K_BITSEL:
            // Every node prints self-delimited (operators in parentheses),
            // so a select can follow any of them directly.
            append_node(out, n->a);
            if (n->msb == n->lsb) {
                g_string_append_printf(out, "[%u]", n->msb);
            } else {
                g_string_append_printf(out, "[%u:%u]", n->msb, n->lsb);
            }
            break;
        default:
            break;
    }
}

gchar *derived_expr_to_string(const DerivedExpr *expr)
{
    GString *out = g_string_new(NULL);
    append_node(out, expr->root);
    return g_string_free(out, FALSE);
}

/* ------------------------------------------------------------- series -- */

DerivedSeries *derived_series_new(guint width)
{
    DerivedSeries *s = g_new0(DerivedSeries, 1);
    s->width = width;
    return s;
}

void derived_series_append(DerivedSeries *series, gint64 time, const guint8 *bits)
{
    // Grow geometrically: histories can be millions of entries long.
    if (series->n == series->capacity) {
        series->capacity = series->capacity == 0 ? 16 : series->capacity * 2;
        series->times = g_renew(gint64, series->times, series->capacity);
        series->bits = g_renew(guint8, series->bits, (gsize)series->capacity * series->width);
    }
    series->times[series->n] = time;
    memcpy(series->bits + (gsize)series->n * series->width, bits, series->width);
    series->n++;
}

void derived_series_free(DerivedSeries *series)
{
    if (series == NULL) {
        return;
    }
    g_free(series->times);
    g_free(series->bits);
    g_free(series);
}

/* ------------------------------------------------------------ analysis -- */

typedef struct
{
    const DerivedInput *inputs;
    guint n_inputs;
    GError **error;
    gboolean failed;
} Analysis;

static void a_fail(Analysis *a, DerivedExprError code, const gchar *fmt, ...) G_GNUC_PRINTF(3, 4);

static void a_fail(Analysis *a, DerivedExprError code, const gchar *fmt, ...)
{
    if (a->failed) {
        return;
    }
    a->failed = TRUE;
    va_list ap;
    va_start(ap, fmt);
    gchar *msg = g_strdup_vprintf(fmt, ap);
    va_end(ap);
    g_set_error_literal(a->error, DERIVED_EXPR_ERROR, code, msg);
    g_free(msg);
}

static gboolean is_arith(Op op)
{
    return op == OP_ADD || op == OP_SUB || op == OP_MUL || op == OP_DIV || op == OP_MOD ||
           op == OP_LT || op == OP_LE || op == OP_GT || op == OP_GE || op == OP_EQ || op == OP_NE;
}

static void analyze(DNode *n, Analysis *a)
{
    if (a->failed) {
        return;
    }
    switch (n->kind) {
        case K_LIT:
            n->width = n->lit_width;
            break;
        case K_REF: {
            n->slot = -1;
            for (guint i = 0; i < a->n_inputs; i++) {
                if (g_str_equal(a->inputs[i].name, n->name)) {
                    n->slot = (gint)i;
                    break;
                }
            }
            if (n->slot < 0 || a->inputs[n->slot].series == NULL) {
                a_fail(a, DERIVED_EXPR_ERROR_UNKNOWN_SIGNAL, "no data for signal '%s'", n->name);
                return;
            }
            n->width = a->inputs[n->slot].series->width;
            break;
        }
        case K_UNARY:
            analyze(n->a, a);
            if (a->failed) {
                return;
            }
            if (n->op == OP_NEG && n->a->width > MAX_ARITH_WIDTH) {
                a_fail(a, DERIVED_EXPR_ERROR_WIDTH,
                       "negation is limited to %d-bit operands (this one is %u bits)",
                       MAX_ARITH_WIDTH, n->a->width);
                return;
            }
            n->width = (n->op == OP_NOT || n->op == OP_NEG) ? n->a->width : 1;
            break;
        case K_BINARY:
            analyze(n->a, a);
            analyze(n->b, a);
            if (a->failed) {
                return;
            }
            if (is_arith(n->op) &&
                (n->a->width > MAX_ARITH_WIDTH || n->b->width > MAX_ARITH_WIDTH)) {
                a_fail(a, DERIVED_EXPR_ERROR_WIDTH,
                       "'%s' is limited to %d-bit operands (got %u and %u bits)", op_text(n->op),
                       MAX_ARITH_WIDTH, n->a->width, n->b->width);
                return;
            }
            if (n->op == OP_SHL || n->op == OP_SHR) {
                if (n->b->width > MAX_ARITH_WIDTH) {
                    a_fail(a, DERIVED_EXPR_ERROR_WIDTH, "a shift amount is limited to %d bits",
                           MAX_ARITH_WIDTH);
                    return;
                }
                n->width = n->a->width;
            } else if (n->op == OP_LT || n->op == OP_LE || n->op == OP_GT || n->op == OP_GE ||
                       n->op == OP_EQ || n->op == OP_NE || n->op == OP_LAND || n->op == OP_LOR) {
                n->width = 1;
            } else {
                n->width = MAX(n->a->width, n->b->width);
            }
            break;
        case K_TERNARY:
            analyze(n->a, a);
            analyze(n->b, a);
            analyze(n->c, a);
            if (a->failed) {
                return;
            }
            n->width = MAX(n->b->width, n->c->width);
            break;
        case K_CONCAT: {
            guint total = 0;
            for (guint i = 0; i < n->items->len; i++) {
                DNode *item = g_ptr_array_index(n->items, i);
                analyze(item, a);
                if (a->failed) {
                    return;
                }
                total += item->width;
            }
            if (total > MAX_WIDTH) {
                a_fail(a, DERIVED_EXPR_ERROR_WIDTH, "concatenation is too wide (%u bits)", total);
                return;
            }
            n->width = total;
            break;
        }
        case K_REPL:
            analyze(n->a, a);
            if (a->failed) {
                return;
            }
            if ((guint64)n->a->width * n->count > MAX_WIDTH) {
                a_fail(a, DERIVED_EXPR_ERROR_WIDTH, "replication is too wide");
                return;
            }
            n->width = n->a->width * n->count;
            break;
        case K_BITSEL:
            analyze(n->a, a);
            if (a->failed) {
                return;
            }
            n->width = n->msb - n->lsb + 1;
            break;
        default:
            break;
    }
}

static void alloc_bufs(DNode *n)
{
    if (n == NULL) {
        return;
    }
    g_free(n->buf);
    n->buf = g_new0(guint8, MAX(n->width, 1u));
    alloc_bufs(n->a);
    alloc_bufs(n->b);
    alloc_bufs(n->c);
    if (n->items != NULL) {
        for (guint i = 0; i < n->items->len; i++) {
            alloc_bufs(g_ptr_array_index(n->items, i));
        }
    }
}

static void release_bufs(DNode *n)
{
    if (n == NULL) {
        return;
    }
    g_clear_pointer(&n->buf, g_free);
    release_bufs(n->a);
    release_bufs(n->b);
    release_bufs(n->c);
    if (n->items != NULL) {
        for (guint i = 0; i < n->items->len; i++) {
            release_bufs(g_ptr_array_index(n->items, i));
        }
    }
}

/* ---------------------------------------------------------- evaluation -- */

typedef struct
{
    const DerivedInput *inputs;
    const gint *cursor; // per input: index of the current change, -1 = none yet
} EvalCtx;

// Bit number k (0 = LSB) of a node's value; bits above its width read as 0.
static inline guint8 lbit(const DNode *n, guint k)
{
    return k < n->width ? n->buf[n->width - 1 - k] : (guint8)DERIVED_BIT_0;
}

static inline gboolean is_unknown(guint8 b)
{
    return b >= DERIVED_BIT_X;
}

static gboolean to_u64(const DNode *n, guint64 *out)
{
    guint64 v = 0;
    for (guint i = 0; i < n->width; i++) {
        guint8 b = n->buf[i];
        if (is_unknown(b)) {
            return FALSE;
        }
        v = (v << 1) | b;
    }
    *out = v;
    return TRUE;
}

static void fill(DNode *n, guint8 b)
{
    memset(n->buf, b, n->width);
}

static void put_u64(DNode *n, guint64 v)
{
    for (guint i = 0; i < n->width; i++) {
        guint k = n->width - 1 - i;
        n->buf[i] = (k < 64 && ((v >> k) & 1)) ? DERIVED_BIT_1 : DERIVED_BIT_0;
    }
}

// 1 = definitely true, 0 = definitely false, X otherwise.
static guint8 truth(const DNode *n)
{
    gboolean unknown = FALSE;
    for (guint i = 0; i < n->width; i++) {
        if (n->buf[i] == DERIVED_BIT_1) {
            return DERIVED_BIT_1;
        }
        if (is_unknown(n->buf[i])) {
            unknown = TRUE;
        }
    }
    return unknown ? DERIVED_BIT_X : DERIVED_BIT_0;
}

static guint8 bit_and(guint8 a, guint8 b)
{
    if (a == DERIVED_BIT_0 || b == DERIVED_BIT_0) {
        return DERIVED_BIT_0;
    }
    return (a == DERIVED_BIT_1 && b == DERIVED_BIT_1) ? DERIVED_BIT_1 : DERIVED_BIT_X;
}

static guint8 bit_or(guint8 a, guint8 b)
{
    if (a == DERIVED_BIT_1 || b == DERIVED_BIT_1) {
        return DERIVED_BIT_1;
    }
    return (a == DERIVED_BIT_0 && b == DERIVED_BIT_0) ? DERIVED_BIT_0 : DERIVED_BIT_X;
}

static guint8 bit_xor(guint8 a, guint8 b)
{
    if (is_unknown(a) || is_unknown(b)) {
        return DERIVED_BIT_X;
    }
    return (guint8)(a ^ b);
}

static guint8 bit_not(guint8 a)
{
    return is_unknown(a) ? DERIVED_BIT_X : (guint8)(a ^ 1);
}

static void eval_node(DNode *n, const EvalCtx *ctx);

static void eval_unary(DNode *n, const EvalCtx *ctx)
{
    DNode *a = n->a;
    eval_node(a, ctx);

    switch (n->op) {
        case OP_NOT:
            for (guint i = 0; i < n->width; i++) {
                n->buf[i] = bit_not(a->buf[i]);
            }
            break;
        case OP_LNOT: {
            guint8 t = truth(a);
            n->buf[0] = t == DERIVED_BIT_X ? (guint8)DERIVED_BIT_X : (guint8)(t ^ 1);
            break;
        }
        case OP_NEG: {
            guint64 v;
            if (to_u64(a, &v)) {
                put_u64(n, 0 - v);
            } else {
                fill(n, DERIVED_BIT_X);
            }
            break;
        }
        case OP_RAND:
        case OP_RNAND: {
            guint8 r = DERIVED_BIT_1;
            for (guint i = 0; i < a->width; i++) {
                r = bit_and(r, a->buf[i]);
            }
            n->buf[0] = n->op == OP_RNAND ? bit_not(r) : r;
            break;
        }
        case OP_ROR:
        case OP_RNOR: {
            guint8 r = DERIVED_BIT_0;
            for (guint i = 0; i < a->width; i++) {
                r = bit_or(r, a->buf[i]);
            }
            n->buf[0] = n->op == OP_RNOR ? bit_not(r) : r;
            break;
        }
        case OP_RXOR:
        case OP_RXNOR: {
            guint8 r = DERIVED_BIT_0;
            for (guint i = 0; i < a->width; i++) {
                r = bit_xor(r, a->buf[i]);
            }
            n->buf[0] = n->op == OP_RXNOR ? bit_not(r) : r;
            break;
        }
        default:
            fill(n, DERIVED_BIT_X);
            break;
    }
}

static void eval_binary(DNode *n, const EvalCtx *ctx)
{
    DNode *a = n->a;
    DNode *b = n->b;
    eval_node(a, ctx);
    eval_node(b, ctx);

    switch (n->op) {
        case OP_AND:
        case OP_OR:
        case OP_XOR:
        case OP_XNOR:
            for (guint k = 0; k < n->width; k++) {
                guint8 x = lbit(a, k);
                guint8 y = lbit(b, k);
                guint8 r = n->op == OP_AND ? bit_and(x, y)
                           : n->op == OP_OR ? bit_or(x, y)
                                            : bit_xor(x, y);
                if (n->op == OP_XNOR) {
                    r = bit_not(r);
                }
                n->buf[n->width - 1 - k] = r;
            }
            return;
        case OP_LAND: {
            guint8 x = truth(a), y = truth(b);
            n->buf[0] = (x == DERIVED_BIT_0 || y == DERIVED_BIT_0)
                            ? (guint8)DERIVED_BIT_0
                            : (x == DERIVED_BIT_1 && y == DERIVED_BIT_1) ? (guint8)DERIVED_BIT_1
                                                                         : (guint8)DERIVED_BIT_X;
            return;
        }
        case OP_LOR: {
            guint8 x = truth(a), y = truth(b);
            n->buf[0] = (x == DERIVED_BIT_1 || y == DERIVED_BIT_1)
                            ? (guint8)DERIVED_BIT_1
                            : (x == DERIVED_BIT_0 && y == DERIVED_BIT_0) ? (guint8)DERIVED_BIT_0
                                                                         : (guint8)DERIVED_BIT_X;
            return;
        }
        case OP_SHL:
        case OP_SHR: {
            guint64 amount;
            if (!to_u64(b, &amount)) {
                fill(n, DERIVED_BIT_X);
                return;
            }
            for (guint k = 0; k < n->width; k++) {
                guint8 r = DERIVED_BIT_0;
                if (n->op == OP_SHL) {
                    if (amount <= k) {
                        r = lbit(a, (guint)(k - amount));
                    }
                } else if (amount < G_MAXUINT && (guint64)k + amount < a->width) {
                    r = lbit(a, (guint)(k + amount));
                }
                n->buf[n->width - 1 - k] = r;
            }
            return;
        }
        default:
            break;
    }

    // Arithmetic and comparison: pessimistic about any unknown operand bit.
    guint64 x, y;
    if (!to_u64(a, &x) || !to_u64(b, &y)) {
        fill(n, DERIVED_BIT_X);
        return;
    }
    guint64 r = 0;
    switch (n->op) {
        case OP_ADD: r = x + y; break;
        case OP_SUB: r = x - y; break;
        case OP_MUL: r = x * y; break;
        case OP_DIV:
        case OP_MOD:
            if (y == 0) {
                fill(n, DERIVED_BIT_X);
                return;
            }
            r = n->op == OP_DIV ? x / y : x % y;
            break;
        case OP_LT: r = x < y; break;
        case OP_LE: r = x <= y; break;
        case OP_GT: r = x > y; break;
        case OP_GE: r = x >= y; break;
        case OP_EQ: r = x == y; break;
        case OP_NE: r = x != y; break;
        default: break;
    }
    put_u64(n, r);
}

static void eval_node(DNode *n, const EvalCtx *ctx)
{
    switch (n->kind) {
        case K_LIT:
            memcpy(n->buf, n->lit_bits, n->width);
            break;
        case K_REF: {
            const DerivedSeries *s = ctx->inputs[n->slot].series;
            gint idx = ctx->cursor[n->slot];
            if (idx < 0) {
                fill(n, DERIVED_BIT_X);
            } else {
                memcpy(n->buf, s->bits + (gsize)idx * s->width, n->width);
            }
            break;
        }
        case K_UNARY:
            eval_unary(n, ctx);
            break;
        case K_BINARY:
            eval_binary(n, ctx);
            break;
        case K_TERNARY: {
            eval_node(n->a, ctx);
            eval_node(n->b, ctx);
            eval_node(n->c, ctx);
            guint8 t = truth(n->a);
            for (guint k = 0; k < n->width; k++) {
                guint8 y = lbit(n->b, k), z = lbit(n->c, k);
                guint8 r = t == DERIVED_BIT_1 ? y : t == DERIVED_BIT_0 ? z
                           : (y == z && !is_unknown(y))               ? y
                                                                      : (guint8)DERIVED_BIT_X;
                n->buf[n->width - 1 - k] = r;
            }
            break;
        }
        case K_CONCAT: {
            guint pos = 0;
            for (guint i = 0; i < n->items->len; i++) {
                DNode *item = g_ptr_array_index(n->items, i);
                eval_node(item, ctx);
                memcpy(n->buf + pos, item->buf, item->width);
                pos += item->width;
            }
            break;
        }
        case K_REPL:
            eval_node(n->a, ctx);
            for (guint r = 0; r < n->count; r++) {
                memcpy(n->buf + (gsize)r * n->a->width, n->a->buf, n->a->width);
            }
            break;
        case K_BITSEL:
            eval_node(n->a, ctx);
            for (guint k = 0; k < n->width; k++) {
                guint src = n->lsb + k;
                // Out of range reads as X, like Verilog.
                n->buf[n->width - 1 - k] =
                    src < n->a->width ? lbit(n->a, src) : (guint8)DERIVED_BIT_X;
            }
            break;
        default:
            break;
    }
}

/* -------------------------------------------------------------- driver -- */

gboolean derived_expr_width(const DerivedExpr *expr,
                            const DerivedInput *inputs,
                            guint n_inputs,
                            guint *out_width,
                            GError **error)
{
    Analysis a = {.inputs = inputs, .n_inputs = n_inputs, .error = error};
    analyze(expr->root, &a);
    if (a.failed) {
        return FALSE;
    }
    *out_width = expr->root->width;
    return TRUE;
}

DerivedSeries *derived_expr_evaluate(const DerivedExpr *expr,
                                     const DerivedInput *inputs,
                                     guint n_inputs,
                                     GError **error)
{
    DNode *root = expr->root;
    Analysis a = {.inputs = inputs, .n_inputs = n_inputs, .error = error};
    analyze(root, &a);
    if (a.failed) {
        return NULL;
    }
    alloc_bufs(root);

    DerivedSeries *out = derived_series_new(root->width);
    gint *cursor = g_new(gint, MAX(n_inputs, 1u));
    for (guint i = 0; i < n_inputs; i++) {
        cursor[i] = -1;
    }
    EvalCtx ctx = {.inputs = inputs, .cursor = cursor};
    guint8 *last = g_new0(guint8, MAX(root->width, 1u));
    gboolean have_last = FALSE;

    // A constant expression has no input to time it by: one value from t=0.
    gboolean any_input = FALSE;
    for (guint i = 0; i < n_inputs; i++) {
        if (inputs[i].series != NULL && inputs[i].series->n > 0) {
            any_input = TRUE;
        }
    }
    if (!any_input) {
        eval_node(root, &ctx);
        derived_series_append(out, 0, root->buf);
    }

    for (;;) {
        // Next time at which any input changes.
        gboolean found = FALSE;
        gint64 next = 0;
        for (guint i = 0; i < n_inputs; i++) {
            const DerivedSeries *s = inputs[i].series;
            if (s == NULL) {
                continue;
            }
            guint ci = (guint)(cursor[i] + 1);
            if (ci < s->n && (!found || s->times[ci] < next)) {
                next = s->times[ci];
                found = TRUE;
            }
        }
        if (!found) {
            break;
        }
        for (guint i = 0; i < n_inputs; i++) {
            const DerivedSeries *s = inputs[i].series;
            while (s != NULL && (guint)(cursor[i] + 1) < s->n && s->times[cursor[i] + 1] == next) {
                cursor[i]++;
            }
        }

        eval_node(root, &ctx);
        if (!have_last || memcmp(last, root->buf, root->width) != 0) {
            derived_series_append(out, next, root->buf);
            memcpy(last, root->buf, root->width);
            have_last = TRUE;
        }
    }

    g_free(last);
    g_free(cursor);
    release_bufs(root);
    return out;
}

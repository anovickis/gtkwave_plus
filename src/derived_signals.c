#include <config.h>
#include <gtkwave.h>
#include <string.h>
#include "derived_signals.h"
#include "derived_expr.h"
#include "analyzer.h"
#include "globals.h"
#include "status.h"

#define REGISTRY_KEY "gtkwave-derived-signals"

typedef struct
{
    gchar *name; // what the trace is called
    gchar *expression; // canonical text, what's saved
    GwNode *node;
    GwHistEnt *entries; // every history entry of the node, one block
    guint8 *vectors; // backing store for h_vector pointers, one block
} DerivedSignal;

static void derived_signal_free(gpointer data)
{
    DerivedSignal *d = data;
    if (d->node != NULL) {
        // AddNodeTraceReturn() allocates this with malloc_2().
        if (d->node->harray != NULL) {
            free_2(d->node->harray);
        }
        g_free(d->node->nname);
        g_free(d->node);
    }
    g_free(d->entries);
    g_free(d->vectors);
    g_free(d->name);
    g_free(d->expression);
    g_free(d);
}

// Node -> DerivedSignal*, owned by (and freed with) the dump file.
static GHashTable *registry(gboolean create)
{
    GwDumpFile *dump = GLOBALS->dump_file;
    if (dump == NULL) {
        return NULL;
    }
    GHashTable *table = g_object_get_data(G_OBJECT(dump), REGISTRY_KEY);
    if (table == NULL && create) {
        table = g_hash_table_new_full(g_direct_hash, g_direct_equal, NULL, derived_signal_free);
        g_object_set_data_full(G_OBJECT(dump), REGISTRY_KEY, table,
                               (GDestroyNotify)g_hash_table_unref);
    }
    return table;
}

gboolean derived_signal_is_derived(GwNode *node)
{
    GHashTable *table = registry(FALSE);
    return node != NULL && table != NULL && g_hash_table_contains(table, node);
}

/* ------------------------------------------------------ resolve / read -- */

typedef struct
{
    GHashTable *aliases; // short name -> canonical name; may be NULL
} ResolveCtx;

static gchar *resolve_signal(const gchar *name, gpointer user_data)
{
    const ResolveCtx *ctx = user_data;
    if (ctx->aliases != NULL) {
        const gchar *target = g_hash_table_lookup(ctx->aliases, name);
        if (target != NULL) {
            return g_strdup(target);
        }
    }
    GwSymbol *sym = gw_dump_file_lookup_symbol(GLOBALS->dump_file, name);
    if (sym == NULL || sym->n == NULL) {
        return NULL;
    }
    // The symbol's own name is the canonical one (an undecorated "top.bus"
    // resolves to "top.bus[3:0]").
    return g_strdup(sym->name);
}

static guint8 to_derived_bit(guint8 raw)
{
    switch ((GwBit)(raw & GW_BIT_MASK)) {
        case GW_BIT_0:
        case GW_BIT_L:
            return DERIVED_BIT_0;
        case GW_BIT_1:
        case GW_BIT_H:
            return DERIVED_BIT_1;
        case GW_BIT_Z:
            return DERIVED_BIT_Z;
        default:
            return DERIVED_BIT_X;
    }
}

// Copies a node's change history into a plain series. Entries before the
// dump starts and the end-of-history sentinels aren't changes.
static DerivedSeries *series_from_node(GwNode *nd, const gchar *name, GError **error)
{
    if (nd->mv.mvlfac != NULL) {
        import_trace(nd); // lazily-loaded formats (FST) only read it on demand
    }

    guint width = 1;
    if (nd->extvals) {
        gint span = nd->msi - nd->lsi;
        width = (guint)(span < 0 ? -span : span) + 1;
    }

    DerivedSeries *series = derived_series_new(width);
    guint8 *bits = g_new0(guint8, width);

    for (GwHistEnt *h = nd->head.next; h != NULL; h = h->next) {
        if (h->time < 0) {
            continue;
        }
        if (h->time >= GW_TIME_MAX - 1) {
            break;
        }
        if (h->flags & (GW_HIST_ENT_FLAG_REAL | GW_HIST_ENT_FLAG_STRING)) {
            g_set_error(error, DERIVED_EXPR_ERROR, DERIVED_EXPR_ERROR_UNSUPPORTED,
                        "'%s' is a real- or string-valued signal; derived signals only work on "
                        "bit and vector signals.",
                        name);
            g_free(bits);
            derived_series_free(series);
            return NULL;
        }

        if (width == 1 && !nd->extvals) {
            bits[0] = to_derived_bit(h->v.h_val);
        } else {
            for (guint i = 0; i < width; i++) {
                bits[i] = to_derived_bit((guint8)h->v.h_vector[i]);
            }
        }

        // Two entries at one time (a glitch) collapse to the later value:
        // a series is strictly ascending.
        if (series->n > 0 && series->times[series->n - 1] == (gint64)h->time) {
            memcpy(series->bits + (gsize)(series->n - 1) * width, bits, width);
        } else {
            derived_series_append(series, (gint64)h->time, bits);
        }
    }
    g_free(bits);
    return series;
}

/* ---------------------------------------------------------- node build -- */

static guint8 to_gw_bit(guint8 d)
{
    switch (d) {
        case DERIVED_BIT_0: return GW_BIT_0;
        case DERIVED_BIT_1: return GW_BIT_1;
        case DERIVED_BIT_Z: return GW_BIT_Z;
        default: return GW_BIT_X;
    }
}

// Lays the series out the way the loaders do: head(-2) -> front cap(-1, X)
// -> changes -> end cap(X) -> sentinel(Z), so every consumer (bsearch,
// the draw loops, value readout) sees a familiar shape.
static DerivedSignal *build_signal(const gchar *name, const gchar *expression,
                                   const DerivedSeries *series)
{
    DerivedSignal *d = g_new0(DerivedSignal, 1);
    d->name = g_strdup(name);
    d->expression = g_strdup(expression);

    guint width = series->width;
    gboolean vector = width > 1;
    guint count = series->n + 4; // head, front cap, end cap, sentinel
    d->entries = g_new0(GwHistEnt, count);
    if (vector) {
        d->vectors = g_new0(guint8, (gsize)count * width);
    }

    for (guint i = 0; i < count; i++) {
        GwHistEnt *e = &d->entries[i];
        e->next = i + 1 < count ? &d->entries[i + 1] : NULL;
        if (vector) {
            e->v.h_vector = (char *)(d->vectors + (gsize)i * width);
        }
    }

    // Entries: [0]=head [1]=front cap, [2..n+1]=changes, [n+2]=end cap, [n+3]=sentinel.
    // (head is embedded in the node itself below; slot 0 just keeps indexing simple.)
    guint last = count - 1;
    for (guint i = 0; i < count; i++) {
        GwHistEnt *e = &d->entries[i];
        gboolean is_change = i >= 2 && i < 2 + series->n;
        if (is_change) {
            guint k = i - 2;
            e->time = (GwTime)series->times[k];
            for (guint b = 0; b < width; b++) {
                guint8 gb = to_gw_bit(series->bits[(gsize)k * width + b]);
                if (vector) {
                    d->vectors[(gsize)i * width + b] = gb;
                } else {
                    e->v.h_val = gb;
                }
            }
        } else {
            guint8 fill = (i == last) ? GW_BIT_Z : GW_BIT_X;
            e->time = i == 0 ? -2 : i == 1 ? -1 : i == last ? GW_TIME_MAX : GW_TIME_MAX - 1;
            if (vector) {
                memset(d->vectors + (gsize)i * width, fill, width);
            } else {
                e->v.h_val = fill;
            }
        }
    }

    GwNode *nd = g_new0(GwNode, 1);
    nd->nname = g_strdup(name);
    nd->extvals = vector ? 1 : 0;
    nd->msi = vector ? (gint)width - 1 : 0;
    nd->lsi = 0;
    nd->vartype = GW_VAR_TYPE_VCD_WIRE;
    nd->mv.mvlfac = NULL; // already "imported"
    nd->head = d->entries[0];
    nd->head.next = &d->entries[1];
    if (vector) {
        // The embedded head needs its own value storage; point it at slot 0's.
        nd->head.v.h_vector = (char *)d->vectors;
    }
    d->node = nd;
    return d;
}

/* --------------------------------------------------------------- add -- */

// Parses `expression` and evaluates it over the loaded dump. On success
// returns the result history and, if `canonical` is non-NULL, the canonical
// expression text.
static DerivedSeries *evaluate_text(const gchar *expression, GHashTable *aliases,
                                    gchar **canonical, GError **error)
{
    if (GLOBALS->dump_file == NULL) {
        g_set_error_literal(error, DERIVED_EXPR_ERROR, DERIVED_EXPR_ERROR_UNSUPPORTED,
                            "No dump file is loaded.");
        return NULL;
    }

    ResolveCtx ctx = {.aliases = aliases};
    DerivedExpr *expr = derived_expr_parse(expression, resolve_signal, &ctx, error);
    if (expr == NULL) {
        return NULL;
    }

    GPtrArray *refs = derived_expr_references(expr);
    DerivedSeries **series = g_new0(DerivedSeries *, MAX(refs->len, 1u));
    DerivedInput *inputs = g_new0(DerivedInput, MAX(refs->len, 1u));
    gboolean ok = TRUE;

    for (guint i = 0; ok && i < refs->len; i++) {
        const gchar *ref = g_ptr_array_index(refs, i);
        GwSymbol *sym = gw_dump_file_lookup_symbol(GLOBALS->dump_file, ref);
        if (sym == NULL || sym->n == NULL) {
            g_set_error(error, DERIVED_EXPR_ERROR, DERIVED_EXPR_ERROR_UNKNOWN_SIGNAL,
                        "no signal named '%s'", ref);
            ok = FALSE;
            break;
        }
        if (derived_signal_is_derived(sym->n)) {
            g_set_error(error, DERIVED_EXPR_ERROR, DERIVED_EXPR_ERROR_UNSUPPORTED,
                        "'%s' is itself a derived signal; derived signals can't be built from "
                        "other derived signals.",
                        ref);
            ok = FALSE;
            break;
        }
        series[i] = series_from_node(sym->n, ref, error);
        if (series[i] == NULL) {
            ok = FALSE;
            break;
        }
        inputs[i].name = ref;
        inputs[i].series = series[i];
    }

    DerivedSeries *result = NULL;
    if (ok) {
        result = derived_expr_evaluate(expr, inputs, refs->len, error);
    }
    if (result != NULL && canonical != NULL) {
        *canonical = derived_expr_to_string(expr);
    }

    for (guint i = 0; i < refs->len; i++) {
        derived_series_free(series[i]);
    }
    g_free(series);
    g_free(inputs);
    g_ptr_array_unref(refs);
    derived_expr_free(expr);
    return result;
}

DerivedSeries *derived_signal_evaluate(const gchar *expression, GHashTable *aliases, GError **error)
{
    return evaluate_text(expression, aliases, NULL, error);
}

static gboolean compute_signal(const gchar *name, const gchar *expression, GHashTable *aliases,
                               DerivedSignal **out, gchar **out_canonical, GError **error)
{
    gchar *canonical = NULL;
    DerivedSeries *result = evaluate_text(expression, aliases, &canonical, error);
    if (result == NULL) {
        return FALSE;
    }
    const gchar *shown = (name != NULL && name[0] != '\0') ? name : expression;
    *out = build_signal(shown, canonical, result);
    *out_canonical = canonical;
    derived_series_free(result);
    return TRUE;
}

gboolean derived_signal_add(const gchar *name, const gchar *expression, GHashTable *aliases,
                            GError **error)
{
    DerivedSignal *d = NULL;
    gchar *canonical = NULL;
    if (!compute_signal(name, expression, aliases, &d, &canonical, error)) {
        return FALSE;
    }
    g_free(canonical);

    GHashTable *table = registry(TRUE);
    g_hash_table_insert(table, d->node, d);

    // AddNodeTraceReturn() builds the history index and picks the default
    // radix, exactly as for a loaded signal.
    if (!AddNodeTraceReturn(d->node, d->name, NULL)) {
        g_hash_table_remove(table, d->node);
        g_set_error_literal(error, DERIVED_EXPR_ERROR, DERIVED_EXPR_ERROR_UNSUPPORTED,
                            "Couldn't add the signal to the waveform.");
        return FALSE;
    }
    return TRUE;
}

/* ------------------------------------------------------------- saving -- */

static void write_quoted(FILE *out, const gchar *s)
{
    gchar *esc = g_strescape(s, NULL);
    fprintf(out, " \"%s\"", esc);
    g_free(esc);
}

gboolean derived_signal_write_line(FILE *out, GwNode *node)
{
    GHashTable *table = registry(FALSE);
    const DerivedSignal *d = table != NULL ? g_hash_table_lookup(table, node) : NULL;
    if (d == NULL) {
        return FALSE;
    }
    fprintf(out, "[derived_signal]");
    write_quoted(out, d->name);
    write_quoted(out, d->expression);
    fputc('\n', out);
    return TRUE;
}

static gchar *read_quoted(const gchar **p)
{
    const gchar *s = *p;
    while (*s == ' ') {
        s++;
    }
    if (*s != '"') {
        return NULL;
    }
    s++;
    const gchar *start = s;
    while (*s != '\0' && *s != '"') {
        if (*s == '\\' && s[1] != '\0') {
            s++;
        }
        s++;
    }
    if (*s != '"') {
        return NULL;
    }
    gchar *raw = g_strndup(start, (gsize)(s - start));
    gchar *out = g_strcompress(raw);
    g_free(raw);
    *p = s + 1;
    return out;
}

gboolean derived_signal_restore_line(const gchar *args)
{
    const gchar *p = args;
    gchar *name = read_quoted(&p);
    gchar *expression = name != NULL ? read_quoted(&p) : NULL;
    if (expression == NULL) {
        g_free(name);
        status_text("Ignored a malformed derived_signal line in the save file.\n");
        return FALSE;
    }

    GError *error = NULL;
    gboolean ok = derived_signal_add(name, expression, NULL, &error);
    if (!ok) {
        gchar *msg = g_strdup_printf("Couldn't restore derived signal '%s': %s\n", name,
                                     error != NULL ? error->message : "unknown error");
        status_text(msg);
        g_free(msg);
        g_clear_error(&error);
    }
    g_free(name);
    g_free(expression);
    return ok;
}

/* ------------------------------------------------------------- dialog -- */

typedef struct
{
    GtkWidget *window;
    GtkWidget *name_entry;
    GtkWidget *expr_entry;
    GtkWidget *error_label;
    GHashTable *aliases; // short name -> canonical name
} DialogState;

static void dialog_state_free(gpointer data)
{
    DialogState *state = data;
    g_hash_table_unref(state->aliases);
    g_free(state);
}

static gchar *leaf_of(const gchar *full)
{
    gchar *base = g_strdup(full);
    gchar *bracket = strchr(base, '[');
    if (bracket != NULL) {
        *bracket = '\0';
    }
    const gchar *dot = strrchr(base, '.');
    gchar *leaf = g_strdup(dot != NULL ? dot + 1 : base);
    g_free(base);
    return leaf;
}

// Offers every highlighted plain signal under its leaf name (when that's
// unique among them) and as s1, s2, ...
GHashTable *derived_signal_highlight_aliases(GString *listing)
{
    GHashTable *aliases = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
    GHashTable *leaf_count = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    GPtrArray *chosen = g_ptr_array_new();
    for (GwTrace *t = GLOBALS->traces.first; t != NULL; t = t->t_next) {
        if ((t->flags & TR_HIGHLIGHT) && !(t->flags & (TR_BLANK | TR_ANALOG_BLANK_STRETCH)) &&
            !t->vector && t->n.nd != NULL && !derived_signal_is_derived(t->n.nd)) {
            g_ptr_array_add(chosen, t->n.nd);
            gchar *leaf = leaf_of(t->n.nd->nname);
            gpointer n = g_hash_table_lookup(leaf_count, leaf);
            g_hash_table_insert(leaf_count, leaf, GUINT_TO_POINTER(GPOINTER_TO_UINT(n) + 1));
        }
    }
    for (guint i = 0; i < chosen->len; i++) {
        GwNode *nd = g_ptr_array_index(chosen, i);
        gchar *leaf = leaf_of(nd->nname);
        gchar *sn = g_strdup_printf("s%u", i + 1);
        g_hash_table_insert(aliases, g_strdup(sn), g_strdup(nd->nname));
        if (GPOINTER_TO_UINT(g_hash_table_lookup(leaf_count, leaf)) == 1) {
            g_hash_table_insert(aliases, g_strdup(leaf), g_strdup(nd->nname));
            g_string_append_printf(listing, "  %s  (or %s)  =  %s\n", leaf, sn, nd->nname);
        } else {
            g_string_append_printf(listing, "  %s  =  %s\n", sn, nd->nname);
        }
        g_free(leaf);
        g_free(sn);
    }
    g_hash_table_unref(leaf_count);
    g_ptr_array_unref(chosen);
    return aliases;
}

static void on_apply(GtkWidget *widget, gpointer data)
{
    (void)widget;
    DialogState *state = data;

    const gchar *expr = gtk_entry_get_text(GTK_ENTRY(state->expr_entry));
    const gchar *name = gtk_entry_get_text(GTK_ENTRY(state->name_entry));

    GError *error = NULL;
    if (!derived_signal_add(name, expr, state->aliases, &error)) {
        gtk_label_set_text(GTK_LABEL(state->error_label), error->message);
        g_clear_error(&error);
        return;
    }
    gtk_widget_destroy(state->window);
}

static void on_cancel(GtkWidget *widget, gpointer data)
{
    (void)widget;
    DialogState *state = data;
    gtk_widget_destroy(state->window);
}

void derived_signal_show_dialog(void)
{
    if (GLOBALS->dump_file == NULL) {
        status_text("Derived signals need a loaded dump file.\n");
        return;
    }

    DialogState *state = g_new0(DialogState, 1);
    GString *listing = g_string_new(NULL);
    state->aliases = derived_signal_highlight_aliases(listing);

    state->window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(state->window), "Add Derived Signal");
    gtk_window_set_default_size(GTK_WINDOW(state->window), 560, -1);
    g_object_set_data_full(G_OBJECT(state->window), "derived-dialog", state, dialog_state_free);

    GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_container_set_border_width(GTK_CONTAINER(vbox), 10);
    gtk_container_add(GTK_CONTAINER(state->window), vbox);

    GtkWidget *grid = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(grid), 6);
    gtk_grid_set_column_spacing(GTK_GRID(grid), 8);
    gtk_box_pack_start(GTK_BOX(vbox), grid, FALSE, FALSE, 0);

    GtkWidget *expr_label = gtk_label_new("Expression:");
    gtk_widget_set_halign(expr_label, GTK_ALIGN_END);
    gtk_grid_attach(GTK_GRID(grid), expr_label, 0, 0, 1, 1);
    state->expr_entry = gtk_entry_new();
    gtk_widget_set_hexpand(state->expr_entry, TRUE);
    gtk_entry_set_activates_default(GTK_ENTRY(state->expr_entry), FALSE);
    g_signal_connect(state->expr_entry, "activate", G_CALLBACK(on_apply), state);
    gtk_grid_attach(GTK_GRID(grid), state->expr_entry, 1, 0, 1, 1);

    GtkWidget *name_label = gtk_label_new("Name:");
    gtk_widget_set_halign(name_label, GTK_ALIGN_END);
    gtk_grid_attach(GTK_GRID(grid), name_label, 0, 1, 1, 1);
    state->name_entry = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(state->name_entry),
                                   "(optional -- defaults to the expression)");
    g_signal_connect(state->name_entry, "activate", G_CALLBACK(on_apply), state);
    gtk_grid_attach(GTK_GRID(grid), state->name_entry, 1, 1, 1, 1);

    gchar *help = g_strdup_printf(
        "%s%s\nOperators (Verilog precedence): ~ ! - & | ^ (unary/reduction)  * / %%  + -  << >>  "
        "< <= > >=  == !=  &  ^  |  &&  ||  ?:\nAlso {a, b}, {4{a}}, a[3], a[7:4], literals like "
        "8'hFF and 4'b10x1. Use \"quotes\" around names with odd characters. Results are as "
        "wide as the widest operand; arithmetic is limited to 64 bits.",
        listing->len > 0 ? "Highlighted signals you can refer to by short name:\n" : "",
        listing->len > 0 ? listing->str : "Tip: highlight signals first to refer to them by "
                                          "short name; full names (top.u.count) always work.\n");
    GtkWidget *help_label = gtk_label_new(help);
    g_free(help);
    gtk_label_set_xalign(GTK_LABEL(help_label), 0.0);
    gtk_label_set_line_wrap(GTK_LABEL(help_label), TRUE);
    gtk_label_set_selectable(GTK_LABEL(help_label), TRUE);
    gtk_box_pack_start(GTK_BOX(vbox), help_label, FALSE, FALSE, 4);
    g_string_free(listing, TRUE);

    state->error_label = gtk_label_new("");
    gtk_label_set_xalign(GTK_LABEL(state->error_label), 0.0);
    gtk_label_set_line_wrap(GTK_LABEL(state->error_label), TRUE);
    gtk_box_pack_start(GTK_BOX(vbox), state->error_label, FALSE, FALSE, 0);

    GtkWidget *buttons = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_widget_set_halign(buttons, GTK_ALIGN_END);
    GtkWidget *cancel = gtk_button_new_with_label("Cancel");
    GtkWidget *apply = gtk_button_new_with_label("Add");
    g_signal_connect(cancel, "clicked", G_CALLBACK(on_cancel), state);
    g_signal_connect(apply, "clicked", G_CALLBACK(on_apply), state);
    gtk_box_pack_end(GTK_BOX(buttons), apply, FALSE, FALSE, 0);
    gtk_box_pack_end(GTK_BOX(buttons), cancel, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(vbox), buttons, FALSE, FALSE, 0);

    gtk_widget_show_all(state->window);
    gtk_widget_grab_focus(state->expr_entry);
}

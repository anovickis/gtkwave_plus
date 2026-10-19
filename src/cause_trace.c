#include "cause_trace.h"

#include <string.h>

CauseHistory *cause_history_new(void)
{
    return g_new0(CauseHistory, 1);
}

void cause_history_append(CauseHistory *h, gint64 time, const gchar *value)
{
    if (h->n > 0) {
        if (time <= h->times[h->n - 1]) {
            // Two values at one time: the later one wins.
            g_free(h->values[h->n - 1]);
            h->values[h->n - 1] = g_strdup(value);
            if (h->n > 1 && g_str_equal(h->values[h->n - 2], value)) {
                g_free(h->values[h->n - 1]);
                h->n--;
            }
            return;
        }
        if (g_str_equal(h->values[h->n - 1], value)) {
            return;
        }
    }
    h->times = g_renew(gint64, h->times, h->n + 1);
    h->values = g_renew(gchar *, h->values, h->n + 1);
    h->times[h->n] = time;
    h->values[h->n] = g_strdup(value);
    h->n++;
}

void cause_history_free(CauseHistory *h)
{
    if (h == NULL) {
        return;
    }
    for (guint i = 0; i < h->n; i++) {
        g_free(h->values[i]);
    }
    g_free(h->values);
    g_free(h->times);
    g_free(h);
}

struct _CauseContext
{
    const NetlistGraph *graph;
    gchar *dump_prefix;
    CauseHistoryFunc history;
    gpointer user_data;
    GHashTable *cache; // dump path -> CauseHistory* (NULL value = known missing)
};

CauseContext *cause_context_new(const NetlistGraph *graph,
                                const gchar *dump_prefix,
                                CauseHistoryFunc history,
                                gpointer user_data)
{
    CauseContext *ctx = g_new0(CauseContext, 1);
    ctx->graph = graph;
    ctx->dump_prefix = g_strdup(dump_prefix);
    ctx->history = history;
    ctx->user_data = user_data;
    ctx->cache = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, (GDestroyNotify)cause_history_free);
    return ctx;
}

void cause_context_free(CauseContext *ctx)
{
    if (ctx == NULL) {
        return;
    }
    g_hash_table_unref(ctx->cache);
    g_free(ctx->dump_prefix);
    g_free(ctx);
}

static const CauseHistory *history_of(CauseContext *ctx, const gchar *dump_path)
{
    if (dump_path == NULL) {
        return NULL;
    }
    gpointer value = NULL;
    if (g_hash_table_lookup_extended(ctx->cache, dump_path, NULL, &value)) {
        return value;
    }
    CauseHistory *h = ctx->history(dump_path, ctx->user_data);
    g_hash_table_insert(ctx->cache, g_strdup(dump_path), h);
    return h;
}

// Index of the last change with time <= t, or -1.
static gint last_at_or_before(const CauseHistory *h, gint64 t)
{
    gint lo = 0, hi = (gint)h->n - 1, found = -1;
    while (lo <= hi) {
        gint mid = lo + (hi - lo) / 2;
        if (h->times[mid] <= t) {
            found = mid;
            lo = mid + 1;
        } else {
            hi = mid - 1;
        }
    }
    return found;
}

static gboolean is_unknown(const gchar *v)
{
    return g_str_equal(v, "x") || g_str_equal(v, "z");
}

// SystemVerilog edge rules for a 1-bit value: posedge is 0->1/x/z or x/z->1.
static gboolean is_edge(const gchar *from, const gchar *to, NetlistEdgeKind kind)
{
    if (from == NULL) {
        return FALSE;
    }
    if (kind == NETLIST_EDGE_POSEDGE) {
        return (g_str_equal(from, "0") && !g_str_equal(to, "0")) ||
               (is_unknown(from) && g_str_equal(to, "1"));
    }
    return (g_str_equal(from, "1") && !g_str_equal(to, "1")) ||
           (is_unknown(from) && g_str_equal(to, "0"));
}

static void item_free(gpointer data)
{
    CauseItem *item = data;
    g_free(item->signal);
    g_free(item->dump_path);
    g_free(item->old_value);
    g_free(item->new_value);
    if (item->statements != NULL) {
        g_ptr_array_unref(item->statements);
    }
    g_free(item);
}

void cause_explanation_free(CauseExplanation *e)
{
    if (e == NULL) {
        return;
    }
    g_free(e->signal);
    g_free(e->dump_path);
    g_free(e->old_value);
    g_free(e->new_value);
    g_free(e->note);
    g_ptr_array_unref(e->items);
    g_ptr_array_unref(e->drivers);
    g_free(e);
}

static GPtrArray *copy_locs(const GPtrArray *src)
{
    GPtrArray *out = g_ptr_array_new_with_free_func((GDestroyNotify)netlist_loc_free);
    for (guint i = 0; src != NULL && i < src->len; i++) {
        const NetlistLoc *l = g_ptr_array_index(src, i);
        NetlistLoc *c = g_new0(NetlistLoc, 1);
        c->file = g_strdup(l->file);
        c->line = l->line;
        c->column = l->column;
        g_ptr_array_add(out, c);
    }
    return out;
}

static CauseItem *item_new(CauseContext *ctx, const NetlistSource *src, CauseKind kind)
{
    CauseItem *item = g_new0(CauseItem, 1);
    item->signal = g_strdup(src->signal);
    item->dump_path = netlist_graph_to_dump_path(ctx->graph, ctx->dump_prefix, src->signal);
    item->kind = kind;
    item->statements = copy_locs(src->statements);
    return item;
}

// Fills time/old/new from change `idx` of `h`.
static void item_set_change(CauseItem *item, const CauseHistory *h, gint idx)
{
    item->has_time = TRUE;
    item->moved = idx > 0;
    item->time = h->times[idx];
    g_free(item->old_value);
    g_free(item->new_value);
    item->old_value = idx > 0 ? g_strdup(h->values[idx - 1]) : NULL;
    item->new_value = g_strdup(h->values[idx]);
}

static gint cmp_items(gconstpointer a, gconstpointer b)
{
    const CauseItem *x = *(CauseItem *const *)a;
    const CauseItem *y = *(CauseItem *const *)b;
    if ((x->kind == CAUSE_TRIGGER) != (y->kind == CAUSE_TRIGGER)) {
        return x->kind == CAUSE_TRIGGER ? -1 : 1;
    }
    if (x->likely != y->likely) {
        return x->likely ? -1 : 1;
    }
    if (x->has_time != y->has_time) {
        return x->has_time ? -1 : 1;
    }
    if (x->has_time && x->time != y->time) {
        return x->time > y->time ? -1 : 1;
    }
    return g_strcmp0(x->signal, y->signal);
}

CauseExplanation *cause_explain(CauseContext *ctx, const gchar *netlist_signal, gint64 time)
{
    CauseExplanation *e = g_new0(CauseExplanation, 1);
    e->signal = g_strdup(netlist_signal);
    e->dump_path = netlist_graph_to_dump_path(ctx->graph, ctx->dump_prefix, netlist_signal);
    e->items = g_ptr_array_new_with_free_func(item_free);
    e->drivers = netlist_graph_driver_statements(ctx->graph, netlist_signal);
    e->is_register = netlist_graph_is_register(ctx->graph, netlist_signal);

    GPtrArray *sources = netlist_graph_sources(ctx->graph, netlist_signal);
    if (sources == NULL) {
        e->note = g_strdup("This signal is not in the design's netlist.");
        return e;
    }

    const CauseHistory *own = history_of(ctx, e->dump_path);
    if (own == NULL) {
        e->note = g_strdup("This signal is not in the dump file.");
        g_ptr_array_unref(sources);
        return e;
    }
    gint idx = last_at_or_before(own, time);
    if (idx < 0) {
        e->note = g_strdup("The signal has no value at or before this time.");
        g_ptr_array_unref(sources);
        return e;
    }

    e->found_change = TRUE;
    e->change_time = own->times[idx];
    e->old_value = idx > 0 ? g_strdup(own->values[idx - 1]) : NULL;
    e->new_value = g_strdup(own->values[idx]);
    e->is_primary = sources->len == 0;
    gint64 t = e->change_time;
    if (idx == 0) {
        e->note = g_strdup("This is the signal's first value in the dump (its initial value); "
                           "nothing changed it.");
    }

    // Registers: which edge fired it, and when the previous one was.
    gboolean triggered = FALSE;
    gboolean have_prev = FALSE;
    gint64 prev_edge = 0;
    for (guint i = 0; i < sources->len; i++) {
        const NetlistSource *src = g_ptr_array_index(sources, i);
        if (src->kind == NETLIST_EDGE_DATA) {
            continue;
        }
        gchar *dump = netlist_graph_to_dump_path(ctx->graph, ctx->dump_prefix, src->signal);
        const CauseHistory *h = history_of(ctx, dump);
        CauseItem *item = item_new(ctx, src, CAUSE_NO_HISTORY);
        g_free(dump);
        if (h != NULL) {
            item->kind = CAUSE_HELD;
            gint ci = last_at_or_before(h, t);
            if (ci >= 0) {
                item_set_change(item, h, ci);
                if (h->times[ci] == t && is_edge(item->old_value, item->new_value, src->kind)) {
                    item->kind = CAUSE_TRIGGER;
                    item->likely = TRUE;
                    triggered = TRUE;
                    // Previous edge of the same sense.
                    for (gint k = ci - 1; k > 0; k--) {
                        if (is_edge(h->values[k - 1], h->values[k], src->kind)) {
                            if (!have_prev || h->times[k] > prev_edge) {
                                prev_edge = h->times[k];
                                have_prev = TRUE;
                            }
                            break;
                        }
                    }
                }
            }
        }
        g_ptr_array_add(e->items, item);
    }

    if (e->is_register && !triggered && e->note == NULL) {
        e->note = g_strdup("No clock or reset edge at this time: the change did not come from the "
                           "register's own clock (initial value, force, or a simulator delay).");
    }

    // Data inputs.
    GPtrArray *data = g_ptr_array_new();
    gboolean any_likely = FALSE;
    gboolean registered_view = e->is_register && triggered;
    for (guint i = 0; i < sources->len; i++) {
        const NetlistSource *src = g_ptr_array_index(sources, i);
        if (src->kind != NETLIST_EDGE_DATA) {
            continue;
        }
        gchar *dump = netlist_graph_to_dump_path(ctx->graph, ctx->dump_prefix, src->signal);
        const CauseHistory *h = history_of(ctx, dump);
        g_free(dump);
        CauseItem *item = item_new(ctx, src, CAUSE_NO_HISTORY);
        g_ptr_array_add(data, item);
        if (h == NULL) {
            continue;
        }

        // The flop sampled the value from before its edge.
        gint ci = last_at_or_before(h, registered_view ? t - 1 : t);
        gint at_t = last_at_or_before(h, t);
        gboolean changed_at_t = at_t >= 0 && h->times[at_t] == t && at_t > 0;
        gboolean in_window = ci >= 0 && (!registered_view || !have_prev || h->times[ci] > prev_edge);

        if (registered_view) {
            if (ci >= 0 && in_window && ci > 0) {
                item->kind = CAUSE_SAMPLED;
                item->likely = TRUE;
                item_set_change(item, h, ci);
            } else if (changed_at_t) {
                item->kind = CAUSE_SAME_TIME;
                item_set_change(item, h, at_t);
            } else {
                item->kind = CAUSE_HELD;
                if (ci >= 0) {
                    item_set_change(item, h, ci);
                    g_free(item->old_value);
                    item->old_value = NULL;
                }
            }
        } else {
            if (changed_at_t) {
                item->kind = CAUSE_CHANGED;
                item->likely = TRUE;
                item_set_change(item, h, at_t);
            } else {
                item->kind = CAUSE_HELD;
                if (ci >= 0) {
                    item_set_change(item, h, ci);
                    g_free(item->old_value);
                    item->old_value = NULL;
                }
            }
        }
        any_likely = any_likely || item->likely;
    }

    // Nothing moved at this instant (a delta cycle or delay hides it): offer
    // the input that moved last.
    if (!any_likely && !registered_view) {
        CauseItem *best = NULL;
        for (guint i = 0; i < data->len; i++) {
            CauseItem *item = g_ptr_array_index(data, i);
            if (item->moved && item->time <= t && (best == NULL || item->time > best->time)) {
                best = item;
            }
        }
        if (best != NULL && best->kind == CAUSE_HELD) {
            best->kind = CAUSE_EARLIER;
            best->likely = TRUE;
            if (e->note == NULL && !e->is_primary) {
                e->note = g_strdup("No input changed at this exact time; the input that changed most "
                                   "recently before it is shown (delta cycles are not in the dump).");
            }
        }
    }
    for (guint i = 0; i < data->len; i++) {
        g_ptr_array_add(e->items, g_ptr_array_index(data, i));
    }
    g_ptr_array_unref(data);
    g_ptr_array_sort(e->items, cmp_items);
    g_ptr_array_unref(sources);
    return e;
}

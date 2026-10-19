#include "condition_search.h"

typedef enum
{
    TRUTH_FALSE,
    TRUTH_TRUE,
    TRUTH_UNKNOWN,
} Truth;

static Truth truth_at(const DerivedSeries *s, guint i)
{
    gboolean unknown = FALSE;
    const guint8 *bits = s->bits + (gsize)i * s->width;
    for (guint b = 0; b < s->width; b++) {
        if (bits[b] == DERIVED_BIT_1) {
            return TRUTH_TRUE;
        }
        if (bits[b] == DERIVED_BIT_X || bits[b] == DERIVED_BIT_Z) {
            unknown = TRUE;
        }
    }
    return unknown ? TRUTH_UNKNOWN : TRUTH_FALSE;
}

static void add_span(ConditionResult *r, gint64 start, gint64 end, gboolean open_ended, gint64 min)
{
    if (end - start < min) {
        return;
    }
    ConditionSpan span = {start, end, open_ended};
    g_array_append_val(r->spans, span);
}

ConditionResult *condition_search_run(const DerivedSeries *series, gint64 min_duration, gint64 dump_end)
{
    ConditionResult *r = g_new0(ConditionResult, 1);
    r->spans = g_array_new(FALSE, FALSE, sizeof(ConditionSpan));

    gboolean in_span = FALSE;
    gint64 start = 0;
    for (guint i = 0; i < series->n; i++) {
        Truth t = truth_at(series, i);
        if (t == TRUTH_UNKNOWN) {
            r->unknown_changes++;
        }
        gint64 time = series->times[i];
        if (t == TRUTH_TRUE && !in_span) {
            in_span = TRUE;
            start = time;
        } else if (t != TRUTH_TRUE && in_span) {
            in_span = FALSE;
            add_span(r, start, time, FALSE, min_duration);
        }
    }
    if (in_span) {
        add_span(r, start, MAX(dump_end, start), TRUE, min_duration);
    }
    return r;
}

void condition_result_free(ConditionResult *r)
{
    if (r == NULL) {
        return;
    }
    g_array_unref(r->spans);
    g_free(r);
}

gint condition_next_span(const ConditionResult *r, gint64 time)
{
    for (guint i = 0; i < r->spans->len; i++) {
        if (g_array_index(r->spans, ConditionSpan, i).start > time) {
            return (gint)i;
        }
    }
    return -1;
}

gint condition_prev_span(const ConditionResult *r, gint64 time)
{
    for (gint i = (gint)r->spans->len - 1; i >= 0; i--) {
        if (g_array_index(r->spans, ConditionSpan, i).start < time) {
            return i;
        }
    }
    return -1;
}

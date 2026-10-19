#pragma once

#include <glib.h>

#include "derived_expr.h"

// Condition search: given the result of a derived expression (see
// derived_expr.h), find when it is true. "True" follows Verilog: some bit is
// 1 (an X/Z elsewhere doesn't matter); all zeros is false; unknown bits with
// no 1 are unknown, which counts as false but is reported separately so a
// condition that was merely never evaluated doesn't look like one that
// failed.
//
// Pure glib, no GTK, so it can be unit-tested directly.

typedef struct
{
    gint64 start; // first time the condition is true
    gint64 end; // time it stops being true (or the end of the dump)
    gboolean open_ended; // still true at the end of the dump
} ConditionSpan;

typedef struct
{
    GArray *spans; // ConditionSpan, ascending, never overlapping
    guint unknown_changes; // changes whose value was unknown (X/Z, no 1)
} ConditionResult;

// Intervals where `series` is true and lasts at least `min_duration` time
// units (0 = any, including a single instant), up to `dump_end`. A series
// that never changes yields a span from its first time. Never NULL.
ConditionResult *condition_search_run(const DerivedSeries *series,
                                      gint64 min_duration,
                                      gint64 dump_end);

void condition_result_free(ConditionResult *result);

// Index of the first span starting strictly after `time`, or -1.
gint condition_next_span(const ConditionResult *result, gint64 time);

// Index of the last span starting strictly before `time`, or -1.
gint condition_prev_span(const ConditionResult *result, gint64 time);

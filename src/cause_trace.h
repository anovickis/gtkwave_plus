#pragma once

#include <glib.h>

#include "netlist_graph.h"

// "Why did this signal change here?" -- the time-aware half of driver
// tracing. For a signal and a time, finds the change being explained and
// ranks the signals that feed it (from the netlist graph) by how well
// their own history explains it:
//
//  * a register changes on a clock edge: the edge is the TRIGGER, and the
//    data inputs that explain the new value are the ones that changed
//    between the previous edge and this one (SAMPLED) -- not the ones that
//    changed at the very same instant, which a flop did not see;
//  * anything else changes because an input changed at that instant
//    (CHANGED); if none did (delta-cycle collapse, a delay), the input that
//    changed most recently before it is offered instead (EARLIER);
//  * inputs that did not move are listed too (HELD) with their value,
//    because a held enable or select is often the real answer.
//
// Each cause carries the time of its own last change, so explaining it in
// turn walks the chain back towards the stimulus. Dump files carry no
// delta-cycle order, so a cause at the same timestamp as its effect is
// inferred from the design, not observed.
//
// Pure glib, no GTK: the signal histories come from a callback so it can be
// tested with canned data.

// A signal's value changes: values[i] holds from times[i]. Values are text
// as shown to the user; 1-bit signals must be "0", "1", "x" or "z" (that is
// how clock edges are recognised). Consecutive equal values are folded.
typedef struct
{
    guint n;
    gint64 *times; // strictly ascending
    gchar **values;
} CauseHistory;

CauseHistory *cause_history_new(void);
void cause_history_append(CauseHistory *h, gint64 time, const gchar *value);
void cause_history_free(CauseHistory *h);

// History of the signal named `dump_path` in the dump file, or NULL if it
// isn't there. The result is owned by the caller (cache is internal).
typedef CauseHistory *(*CauseHistoryFunc)(const gchar *dump_path, gpointer user_data);

typedef struct _CauseContext CauseContext;

// `dump_prefix` is the dump path of the top module (see
// netlist_graph_resolve_dump_path()). The graph must outlive the context.
CauseContext *cause_context_new(const NetlistGraph *graph,
                                const gchar *dump_prefix,
                                CauseHistoryFunc history,
                                gpointer user_data);
void cause_context_free(CauseContext *ctx);

typedef enum
{
    CAUSE_TRIGGER, // clock/reset edge that fired the register
    CAUSE_SAMPLED, // register data input that changed since the previous edge
    CAUSE_CHANGED, // input that changed at the same time
    CAUSE_EARLIER, // input that changed most recently before
    CAUSE_SAME_TIME, // changed at the register's own edge: a flop did not see it
    CAUSE_HELD, // did not change
    CAUSE_NO_HISTORY, // not in the dump
} CauseKind;

typedef struct
{
    gchar *signal; // netlist path
    gchar *dump_path; // NULL if it can't be named in the dump
    CauseKind kind;
    gboolean likely; // TRIGGER, SAMPLED, CHANGED, EARLIER
    gboolean has_time;
    gint64 time; // of this signal's relevant change (explain it at this time)
    gboolean moved; // time is a real change, not just its initial value
    gchar *old_value; // NULL if it had none
    gchar *new_value; // NULL if no history; for HELD the value it held
    GPtrArray *statements; // NetlistLoc*: where it feeds the target
} CauseItem;

typedef struct
{
    gchar *signal;
    gchar *dump_path;
    gboolean found_change; // FALSE if the signal has no change at or before `time`
    gint64 change_time;
    gchar *old_value; // NULL for the first value in the dump
    gchar *new_value;
    gboolean is_register;
    gboolean is_primary; // no sources: driven from outside the design
    GPtrArray *items; // CauseItem*, trigger/likely causes first
    GPtrArray *drivers; // NetlistLoc*: statements that write the signal
    gchar *note; // NULL or a one-line caveat
} CauseExplanation;

// Explains the change of `netlist_signal` at or most recently before
// `time` (dump time units). Never NULL; a signal that isn't in the netlist
// or the dump comes back with found_change FALSE and a note.
CauseExplanation *cause_explain(CauseContext *ctx, const gchar *netlist_signal, gint64 time);

void cause_explanation_free(CauseExplanation *e);

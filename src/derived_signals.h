#pragma once

#include <glib.h>
#include <stdio.h>

#include "derived_expr.h"
#include "gtkwave.h"

// Derived ("virtual") signals: a user-written expression over existing
// signals (see derived_expr.h for the language), computed across the whole
// dump into a new history and shown as an ordinary signal -- radix, colour,
// analog mode, markers and value readouts all work on it unchanged.
//
// The result is a real GwNode with its own history, owned by a registry
// attached to the loaded dump file, so it's freed with the dump (including
// across File > Reload and Auto Update, which rebuild the dump and recompute
// every derived signal from its saved expression). Derived signals are saved
// in the save file as `[derived_signal] "name" "expression"`, with the
// expression in canonical form (full signal names), so a save file doesn't
// depend on whatever short names were used to type it.
//
// Not supported: real- or string-valued operands; referencing another derived
// signal; editing in place (delete the trace and add it again).

// Creates a derived signal and adds it to the waveform. `name` (may be NULL
// or empty: the expression text is used) is what the trace is called.
// `aliases` (may be NULL) maps a short name usable in the expression to a
// full signal name. Returns FALSE with *error set on a bad expression,
// unknown signal, unsupported operand or width problem.
gboolean derived_signal_add(const gchar *name,
                            const gchar *expression,
                            GHashTable *aliases,
                            GError **error);

// Evaluates an expression over the loaded dump without adding anything to
// the waveform; same language, aliases and errors as derived_signal_add().
// Caller frees with derived_series_free().
DerivedSeries *derived_signal_evaluate(const gchar *expression,
                                       GHashTable *aliases,
                                       GError **error);

// Short names for the highlighted plain signals (leaf name when unique, and
// s1, s2, ...) -> full name, for use as `aliases`. If `listing` is non-NULL
// a human-readable list of them is appended.
GHashTable *derived_signal_highlight_aliases(GString *listing);

// TRUE if `node` is a derived signal's node.
gboolean derived_signal_is_derived(GwNode *node);

// If `node` is derived, writes its `[derived_signal] ...` save-file line to
// `out` and returns TRUE.
gboolean derived_signal_write_line(FILE *out, GwNode *node);

// Recreates a derived signal from the part of a save-file line after
// "[derived_signal] ". Reports a failure through the status line; returns
// FALSE on any problem.
gboolean derived_signal_restore_line(const gchar *args);

// Opens the "Add Derived Signal" dialog, offering the currently highlighted
// signals under short names.
void derived_signal_show_dialog(void);

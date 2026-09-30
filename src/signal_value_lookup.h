#pragma once

#include <config.h>
#include <gtkwave.h>

// Formats the value of the named signal (gtkwave's own dot-delimited full
// hierarchical path) at an arbitrary time, without adding it as a visible
// trace. Returns a newly-allocated string the caller frees with free_2(),
// or NULL if the signal isn't found in the loaded design or has no value
// yet at that time.
gchar *signal_value_at_time(const gchar *signal_path, GwTime time);

// Returns the time of the named signal's next (forward == TRUE) or
// previous (forward == FALSE) real transition relative to `time`, without
// adding it as a visible trace. Returns `time` unchanged if the signal
// isn't found, has no history, or there's no further transition in the
// requested direction (already at the last/first one).
GwTime signal_step_transition_time(const gchar *signal_path, GwTime time, gboolean forward);

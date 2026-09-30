#pragma once

#include <config.h>
#include <gtkwave.h>

// Formats the value of the named signal (gtkwave's own dot-delimited full
// hierarchical path) at an arbitrary time, without adding it as a visible
// trace. Returns a newly-allocated string the caller frees with free_2(),
// or NULL if the signal isn't found in the loaded design or has no value
// yet at that time.
gchar *signal_value_at_time(const gchar *signal_path, GwTime time);

#pragma once

#include <glib.h>

// Auto Update: while enabled, polls the loaded FST dump (default every 2s)
// and, once a still-running simulation has flushed new time to it, reloads
// the waveform in place -- same state-preserving path as File > Reload
// Waveform -- and keeps the view pinned to the newest data if that's where
// it was. Only reloads when a cheap probe says the file is readable AND
// its end time changed (see auto_update_poll.h), so it never blocks
// retrying against a half-written file.
//
// Only FST dumps are supported; VCD's loader isn't resumable. Refuses (with
// a status message) for any other type, and when a skip-start/skip-end
// window was requested, since a reload would silently drop it.

// Turns Auto Update on or off. Returns the resulting state, which is FALSE
// after a refused enable.
gboolean auto_update_set_enabled(gboolean enabled);

gboolean auto_update_is_enabled(void);

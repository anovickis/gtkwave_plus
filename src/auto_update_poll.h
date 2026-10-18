#pragma once

#include <glib.h>

// Decision logic for Auto Update (live-tailing a dump that a simulator is
// still writing). GTK-free so it can be unit-tested against a real,
// still-open FST writer.
//
// An in-progress FST file is only readable once the writer has flushed its
// hierarchy, and then exposes data up to the last *flushed* block -- so
// "has the file grown" can't be answered by size alone (a write can land
// without making new time visible) and "is it readable" can't be assumed
// (the very first polls may fail to open). The poll therefore gates on a
// cheap stat() first, then probes with a real fstReaderOpen() and compares
// the file's own end time.

typedef enum
{
    AUTO_UPDATE_IDLE, // nothing to do this tick
    AUTO_UPDATE_RELOAD, // readable and its end time changed: reload now
} AutoUpdateAction;

typedef struct
{
    gboolean primed; // a baseline has been recorded
    gint64 size;
    gint64 mtime_usec;
    guint64 end_time; // FST end time (file's own units) at the last reload/prime
} AutoUpdatePoll;

// Records the file's current state as the baseline, so only growth *after*
// this call triggers a reload. Returns FALSE (leaving the baseline unset)
// if the file can't be read as FST right now.
gboolean auto_update_poll_prime(AutoUpdatePoll *poll, const gchar *path);

// Call once per timer tick. Never blocks on an unreadable file: it just
// returns IDLE and tries again next tick, without consuming the change.
// On RELOAD the baseline is already advanced to the new state.
AutoUpdateAction auto_update_poll_check(AutoUpdatePoll *poll, const gchar *path);

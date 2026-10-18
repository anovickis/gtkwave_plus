#include "auto_update_poll.h"

#include <glib/gstdio.h>

#include "fstapi.h"

static gboolean stat_file(const gchar *path, gint64 *size, gint64 *mtime_usec)
{
    GStatBuf st;
    if (g_stat(path, &st) != 0) {
        return FALSE;
    }
    *size = (gint64)st.st_size;
    *mtime_usec = (gint64)st.st_mtim.tv_sec * G_USEC_PER_SEC + st.st_mtim.tv_nsec / 1000;
    return TRUE;
}

// fstReaderOpen() returns NULL for a file whose hierarchy hasn't been
// written yet, which is exactly the "not readable yet" case.
static gboolean probe_end_time(const gchar *path, guint64 *end_time)
{
    fstReaderContext *reader = fstReaderOpen(path);
    if (reader == NULL) {
        return FALSE;
    }
    *end_time = (guint64)fstReaderGetEndTime(reader);
    fstReaderClose(reader);
    return TRUE;
}

gboolean auto_update_poll_prime(AutoUpdatePoll *poll, const gchar *path)
{
    gint64 size, mtime;
    guint64 end;

    if (!stat_file(path, &size, &mtime) || !probe_end_time(path, &end)) {
        return FALSE;
    }
    poll->size = size;
    poll->mtime_usec = mtime;
    poll->end_time = end;
    poll->primed = TRUE;
    return TRUE;
}

AutoUpdateAction auto_update_poll_check(AutoUpdatePoll *poll, const gchar *path)
{
    gint64 size, mtime;

    if (!stat_file(path, &size, &mtime)) {
        return AUTO_UPDATE_IDLE;
    }

    if (!poll->primed) {
        auto_update_poll_prime(poll, path);
        return AUTO_UPDATE_IDLE;
    }

    if (size == poll->size && mtime == poll->mtime_usec) {
        return AUTO_UPDATE_IDLE;
    }

    guint64 end;
    if (!probe_end_time(path, &end)) {
        // Changed but unreadable right now (mid-write): leave the baseline
        // alone so the next tick looks again.
        return AUTO_UPDATE_IDLE;
    }

    poll->size = size;
    poll->mtime_usec = mtime;

    // != rather than >: a rewritten/truncated file (a new simulation run to
    // the same path) ends earlier and needs a reload just as much.
    if (end != poll->end_time) {
        poll->end_time = end;
        return AUTO_UPDATE_RELOAD;
    }
    return AUTO_UPDATE_IDLE;
}

#include <config.h>
#include <gtkwave.h>
#include "auto_update.h"
#include "auto_update_poll.h"
#include "currenttime.h"
#include "globals.h"
#include "status.h"
#include "time_compress.h"

#define AUTO_UPDATE_INTERVAL_SECONDS 2

static guint timer_id = 0;
static AutoUpdatePoll poll_state;

gboolean auto_update_is_enabled(void)
{
    return timer_id != 0;
}

// Same visible-window update the edge-scroll code in wavewindow.c uses:
// move tims.start, then drive the hslider so everything else follows.
static void scroll_view_to_end(void)
{
    GwTime pageinc = (GwTime)gw_time_page_increment();
    GwTime start = time_trunc(GLOBALS->tims.last - pageinc + 1);
    if (start < GLOBALS->tims.first) {
        start = GLOBALS->tims.first;
    }
    GLOBALS->tims.start = start;

    gtk_adjustment_set_value(GTK_ADJUSTMENT(GLOBALS->wave_hslider),
                             GLOBALS->tims.timecache = GLOBALS->tims.start);
    g_signal_emit_by_name(GTK_ADJUSTMENT(GLOBALS->wave_hslider), "changed");
    g_signal_emit_by_name(GTK_ADJUSTMENT(GLOBALS->wave_hslider), "value_changed");
}

static gboolean tick(gpointer data)
{
    (void)data;

    // Never reload under the user's hands: mid-drag, or while a reload is
    // already underway (it pumps the main loop).
    if (GLOBALS->in_button_press_wavewindow_c_1 || GLOBALS->loaded_file_type != FST_FILE ||
        GLOBALS->loaded_file_name == NULL) {
        return G_SOURCE_CONTINUE;
    }

    if (auto_update_poll_check(&poll_state, GLOBALS->loaded_file_name) != AUTO_UPDATE_RELOAD) {
        return G_SOURCE_CONTINUE;
    }

    // If the view is showing the end of the data, the user is tailing:
    // keep them there. If they've scrolled back to study something, leave
    // their window alone.
    gboolean was_at_end = GLOBALS->tims.end >= GLOBALS->tims.last;

    reload_into_new_context();

    if (was_at_end) {
        scroll_view_to_end();
    }

    status_text("Auto Update: loaded newly written data.\n");
    return G_SOURCE_CONTINUE;
}

gboolean auto_update_set_enabled(gboolean enabled)
{
    if (!enabled) {
        if (timer_id != 0) {
            g_source_remove(timer_id);
            timer_id = 0;
            status_text("Auto Update off.\n");
        }
        return FALSE;
    }

    if (timer_id != 0) {
        return TRUE;
    }

    if (GLOBALS->loaded_file_type != FST_FILE || GLOBALS->loaded_file_name == NULL) {
        status_text("Auto Update only supports FST dump files.\n");
        return FALSE;
    }
    if (GLOBALS->skip_start != NULL || GLOBALS->skip_end != NULL) {
        status_text("Auto Update can't be used with a skip-start/skip-end window.\n");
        return FALSE;
    }

    // Baseline = the file as it is now; only data written after this
    // triggers a reload. If it isn't readable yet, the first successful
    // poll primes it instead.
    memset(&poll_state, 0, sizeof(poll_state));
    auto_update_poll_prime(&poll_state, GLOBALS->loaded_file_name);

    timer_id = g_timeout_add_seconds(AUTO_UPDATE_INTERVAL_SECONDS, tick, NULL);
    status_text("Auto Update on (checking every 2 seconds).\n");
    return TRUE;
}

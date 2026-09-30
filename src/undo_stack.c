#include <config.h>
#include <gtkwave.h>
#include <glib/gstdio.h>
#include "undo_stack.h"
#include "globals.h"
#include "analyzer.h"
#include "savefile.h"
#include "wavewindow.h"
#include "menu.h"
#include "status.h"

/* Bounds memory/disk use -- an undo history of destructive trace-list
 * edits doesn't need to be unbounded, and this is a plain linear stack,
 * not a full command-pattern undo/redo tree. */
#define UNDO_STACK_MAX_DEPTH 10

/* Owned gchar* temp-file paths, oldest first; each holds one
 * write_save_helper() snapshot. */
static GPtrArray *undo_stack = NULL;

static void undo_snapshot_discard(gpointer data)
{
    gchar *path = data;
    g_unlink(path);
    g_free(path);
}

static void ensure_undo_stack(void)
{
    if (undo_stack == NULL) {
        undo_stack = g_ptr_array_new_with_free_func(undo_snapshot_discard);
    }
}

void undo_stack_push_snapshot(void)
{
    ensure_undo_stack();

    gchar *path = NULL;
    gint fd = g_file_open_tmp("gtkwave-undo-XXXXXX.gtkw", &path, NULL);
    if (fd < 0) {
        return;
    }

    FILE *f = fdopen(fd, "wb");
    if (f == NULL) {
        close(fd);
        g_unlink(path);
        g_free(path);
        return;
    }

    write_save_helper(path, f);
    fclose(f);

    g_ptr_array_add(undo_stack, path);

    if (undo_stack->len > UNDO_STACK_MAX_DEPTH) {
        /* Discards the oldest snapshot -- frees and unlinks it via
         * undo_snapshot_discard(), the array's element-free function. */
        g_ptr_array_remove_index(undo_stack, 0);
    }
}

gboolean undo_stack_can_undo(void)
{
    return undo_stack != NULL && undo_stack->len > 0;
}

void undo_stack_undo(void)
{
    if (!undo_stack_can_undo()) {
        status_text("GTKWAVE | Nothing to undo.\n");
        return;
    }

    /* Steal (not remove) so undo_snapshot_discard() doesn't unlink the
     * file out from under read_save_helper() below. */
    gchar *path = g_ptr_array_steal_index(undo_stack, undo_stack->len - 1);

    // Clear current trace-list state before replaying the snapshot --
    // read_save_helper()'s directives (traces, groups) append rather
    // than replace, same as WCP's "clear" command (wcp_gtkwave.c).
    while (GLOBALS->traces.first) {
        RemoveTrace(GLOBALS->traces.first, 1);
    }
    collect_all_named_markers(NULL, 0, NULL);

    // The snapshot has no [dumpfile] line with any effect (parsewavline's
    // own "dumpfile" branch is a deliberate no-op -- only external
    // finder/DnD code looks at it), so this only replays trace/group/
    // marker/color state onto the dump file that's already loaded; it
    // never tries to re-open or re-import anything.
    char is_gtkw_save_file_cached = GLOBALS->is_gtkw_save_file;
    int rc = read_save_helper(path, NULL, NULL, NULL, NULL, NULL);
    GLOBALS->is_gtkw_save_file = is_gtkw_save_file_cached;

    GLOBALS->signalwindow_width_dirty = 1;
    MaxSignalLength();
    redraw_signals_and_waves();

    g_unlink(path);
    g_free(path);

    if (rc < 0) {
        status_text("GTKWAVE | Undo failed: could not reopen the snapshot -- trace list was "
                    "cleared but not restored.\n");
    } else {
        status_text("GTKWAVE | Undo complete.\n");
    }
}

void undo_stack_cleanup(void)
{
    if (undo_stack != NULL) {
        g_ptr_array_free(undo_stack, TRUE);
        undo_stack = NULL;
    }
}

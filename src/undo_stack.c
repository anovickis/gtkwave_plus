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
 * write_save_helper() snapshot. Two independent stacks, identical shape:
 * undoing pushes the pre-undo state onto the redo stack (and vice versa),
 * the same relationship any plain linear undo/redo pair has. */
static GPtrArray *undo_stack = NULL;
static GPtrArray *redo_stack = NULL;

static void snapshot_discard(gpointer data)
{
    gchar *path = data;
    g_unlink(path);
    g_free(path);
}

static GPtrArray *ensure_stack(GPtrArray **stack)
{
    if (*stack == NULL) {
        *stack = g_ptr_array_new_with_free_func(snapshot_discard);
    }
    return *stack;
}

/* Writes the current trace/group/marker/color state to a fresh temp file
 * and pushes its path onto `stack`, evicting the oldest entry past
 * UNDO_STACK_MAX_DEPTH. Best-effort: if the snapshot can't be written,
 * the caller's operation proceeds anyway, it just won't be
 * undoable/redoable. */
static void push_snapshot(GPtrArray **stack)
{
    GPtrArray *s = ensure_stack(stack);

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

    g_ptr_array_add(s, path);

    if (s->len > UNDO_STACK_MAX_DEPTH) {
        /* Discards the oldest snapshot -- frees and unlinks it via
         * snapshot_discard(), the array's element-free function. */
        g_ptr_array_remove_index(s, 0);
    }
}

/* Clears the current trace list/named markers and replays a snapshot
 * file over the already-loaded dump file. Returns read_save_helper()'s
 * raw result (negative on failure to reopen the file). Does not unlink
 * `path` -- the caller owns that. */
static int restore_snapshot(const gchar *path)
{
    // Clear current trace-list state before replaying the snapshot --
    // read_save_helper()'s directives (traces, groups) append rather
    // than replace, same as WCP's "clear" command (wcp_gtkwave.c).
    while (GLOBALS->traces.first) {
        RemoveTrace(GLOBALS->traces.first, 1);
    }
    collect_all_named_markers(NULL, 0, NULL);

    // The snapshot has no [dumpfile] line with any effect (parsewavline()'s
    // own "dumpfile" branch is a deliberate no-op -- only external
    // finder/DnD code looks at it), so this only replays trace/group/
    // marker/color state onto the dump file that's already loaded; it
    // never tries to re-open or re-import anything.
    char is_gtkw_save_file_cached = GLOBALS->is_gtkw_save_file;
    int rc = read_save_helper((char *)path, NULL, NULL, NULL, NULL, NULL);
    GLOBALS->is_gtkw_save_file = is_gtkw_save_file_cached;

    GLOBALS->signalwindow_width_dirty = 1;
    MaxSignalLength();
    redraw_signals_and_waves();

    return rc;
}

void undo_stack_push_snapshot(void)
{
    push_snapshot(&undo_stack);

    // A fresh destructive op invalidates whatever could have been redone
    // -- the same convention any plain linear undo/redo history uses.
    if (redo_stack != NULL) {
        g_ptr_array_set_size(redo_stack, 0);
    }
}

gboolean undo_stack_can_undo(void)
{
    return undo_stack != NULL && undo_stack->len > 0;
}

gboolean undo_stack_can_redo(void)
{
    return redo_stack != NULL && redo_stack->len > 0;
}

void undo_stack_undo(void)
{
    if (!undo_stack_can_undo()) {
        status_text("GTKWAVE | Nothing to undo.\n");
        return;
    }

    // Capture the state being replaced *before* touching anything, so
    // Redo can bring it back -- this has to happen before
    // restore_snapshot() clears the trace list.
    push_snapshot(&redo_stack);

    /* Steal (not remove) so snapshot_discard() doesn't unlink the file
     * out from under restore_snapshot() below. */
    gchar *path = g_ptr_array_steal_index(undo_stack, undo_stack->len - 1);
    int rc = restore_snapshot(path);
    g_unlink(path);
    g_free(path);

    if (rc < 0) {
        status_text("GTKWAVE | Undo failed: could not reopen the snapshot -- trace list was "
                    "cleared but not restored.\n");
    } else {
        status_text("GTKWAVE | Undo complete.\n");
    }
}

void undo_stack_redo(void)
{
    if (!undo_stack_can_redo()) {
        status_text("GTKWAVE | Nothing to redo.\n");
        return;
    }

    // Symmetric with undo_stack_undo(): capture the state being replaced
    // so a subsequent Undo can bring it back again.
    push_snapshot(&undo_stack);

    gchar *path = g_ptr_array_steal_index(redo_stack, redo_stack->len - 1);
    int rc = restore_snapshot(path);
    g_unlink(path);
    g_free(path);

    if (rc < 0) {
        status_text("GTKWAVE | Redo failed: could not reopen the snapshot -- trace list was "
                    "cleared but not restored.\n");
    } else {
        status_text("GTKWAVE | Redo complete.\n");
    }
}

void undo_stack_cleanup(void)
{
    if (undo_stack != NULL) {
        g_ptr_array_free(undo_stack, TRUE);
        undo_stack = NULL;
    }
    if (redo_stack != NULL) {
        g_ptr_array_free(redo_stack, TRUE);
        redo_stack = NULL;
    }
}

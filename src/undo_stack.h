#ifndef UNDO_STACK_H
#define UNDO_STACK_H

#include <glib.h>

/* Snapshots the current trace-list-affecting state (traces, groups,
 * colors, markers -- via the same savefile serialization normal session
 * save/reload already uses) onto a bounded undo stack. Call this right
 * before a destructive trace-list operation (Ungroup, Delete, Combine,
 * Expand); best-effort -- if the snapshot can't be written (e.g. no
 * writable temp dir), the operation proceeds anyway, it just won't be
 * undoable. */
void undo_stack_push_snapshot(void);

/* TRUE if there's a snapshot to restore. */
gboolean undo_stack_can_undo(void);

/* Restores the most recent snapshot, replacing the current trace list
 * (and named markers) with what was captured. No-op with a status
 * message if the stack is empty. */
void undo_stack_undo(void);

/* Discards every remaining snapshot and its temp file. Call once at
 * shutdown. */
void undo_stack_cleanup(void);

#endif /* UNDO_STACK_H */

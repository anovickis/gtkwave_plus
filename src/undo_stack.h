#ifndef UNDO_STACK_H
#define UNDO_STACK_H

#include <glib.h>

/* Snapshots the current trace-list-affecting state (traces, groups,
 * colors, markers -- via the same savefile serialization normal session
 * save/reload already uses) onto a bounded undo stack, and clears
 * whatever was pending on the redo stack (a fresh destructive edit
 * invalidates any previously-undone state, same as any plain linear
 * undo/redo history). Call this right before a destructive trace-list
 * operation (Ungroup, Delete, Combine, Expand); best-effort -- if the
 * snapshot can't be written (e.g. no writable temp dir), the operation
 * proceeds anyway, it just won't be undoable. */
void undo_stack_push_snapshot(void);

/* TRUE if there's a snapshot to restore. */
gboolean undo_stack_can_undo(void);

/* TRUE if there's a previously-undone snapshot to restore. */
gboolean undo_stack_can_redo(void);

/* Restores the most recent snapshot, replacing the current trace list
 * (and named markers) with what was captured -- and pushes the state
 * just replaced onto the redo stack first, so Redo can bring it back.
 * No-op with a status message if the undo stack is empty. */
void undo_stack_undo(void);

/* Inverse of undo_stack_undo(): restores the most recently undone
 * snapshot, pushing the state just replaced back onto the undo stack.
 * No-op with a status message if the redo stack is empty. */
void undo_stack_redo(void);

/* Discards every remaining snapshot (both stacks) and its temp file.
 * Call once at shutdown. */
void undo_stack_cleanup(void);

#endif /* UNDO_STACK_H */

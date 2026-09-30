#ifndef TIME_SPAN_COMMENTS_H
#define TIME_SPAN_COMMENTS_H

#include <config.h>
#include <gtkwave.h>

// Time-span Comments: a range annotation [start, end) plus free text,
// distinct from point markers (single timestamp) and named markers
// (single timestamp + name). A natural complement to markers/groups for
// documenting a debug session -- "this whole stretch is where the bug
// reproduces", not just a single instant.

typedef struct
{
    GwTime start;
    GwTime end;
    gchar *text; /* owned */
} GwTimeSpanComment;

// Adds a new comment (start/end are normalized so start <= end). Takes
// its own copy of `text`.
void time_span_comments_add(GwTime start, GwTime end, const gchar *text);

// Removes the comment at `index` (as returned by
// time_span_comments_get()'s iteration order). No-op if out of range.
void time_span_comments_remove(guint index);

guint time_span_comments_count(void);

// Borrowed pointer, valid until the next add/remove/clear. NULL if
// `index` is out of range.
const GwTimeSpanComment *time_span_comments_get(guint index);

// Discards every comment (used before a fresh session load, same as
// time_compress_clear() for compressed spans).
void time_span_comments_clear(void);

// Opens a small modal dialog to add a new comment -- Start/End time
// entries (pre-filled from the primary/baseline markers if both are
// set, same convention Compress Time Range uses) and a text entry.
void time_span_comments_show_add_dialog(void);

// Opens a list window of every comment, each with a "Go To" (jump
// marker + viewport to the comment's start) and "Delete" button.
void time_span_comments_show_list(void);

#endif /* TIME_SPAN_COMMENTS_H */

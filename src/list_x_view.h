#pragma once

#include <config.h>
#include <gtkwave.h>

typedef struct
{
    gchar *signal_path; /* best-effort display path: t->name_full, or t->name if unavailable */
    gboolean has_full_path; /* TRUE only if t->name_full was non-NULL --
                             * gates the "Trace Drivers" action, since a
                             * bare t->name can be ambiguous/wrong for a
                             * nested scope (same convention
                             * src/signal_popover.c's find_target_trace()
                             * already applies) */
    GwTime start;
    GwTime end; /* exclusive; clamped to GLOBALS->tims.last if there's no next entry */
} ListXEntry;

// Headless variant of the scan list_x_show() displays: returns the owned
// GPtrArray of ListXEntry* directly (free with g_ptr_array_unref()) rather
// than opening a results window. Sets *out_truncated if LIST_X_MAX_ENTRIES
// was hit.
GPtrArray *list_x_scan_all(gboolean *out_truncated);

// "List X": scans every currently-loaded, non-blank trace's full value
// history (across the whole loaded time range) for unknown/X-like values,
// using the same definition src/strace.c's ST_X search type already uses
// (scalar: 'x'/'X'; vector/bus: any 'x'/'X'/'w'/'W' character in the ascii
// value). Lists each contiguous bad-value interval with a "Go To" action
// (jumps the primary marker + viewport there) and a "Trace Drivers" action
// that reuses the existing driver-query backend
// (netlist_query_show_drivers(), see netlist_query_ui.h), seeded from the
// signal found to be X instead of a manually-picked signal name.
void list_x_show(void);

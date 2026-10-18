#pragma once

#include <glib.h>

// Backend for the "Event Sequence" pane: what changed at ONE timestamp, in
// what order, and which of those changes plausibly caused which.
//
// Neither VCD nor FST carries real delta-cycle numbers, and gtkwave's own
// per-node history lists (GwHistEnt) deliberately keep no cross-signal
// ordering, so this is reconstructed on demand from two independent
// sources instead of adding a field to every history entry:
//
//  1. Dump order (VCD only): the dump file is re-scanned for the requested
//     timestamp and the value changes are reported in the order the
//     simulator wrote them. FST can't provide this -- its reader walks
//     each time step's changes by signal handle, not original write
//     order -- so for FST `order_known` is FALSE and seq is 0.
//  2. Causality (any format, needs the design's netlist): each changed
//     signal's combinational fan-in cone is intersected with the set of
//     signals that changed at the same timestamp, giving a "caused by"
//     relation and a causal level (0 = no changed signal feeds it, N =
//     1 + the deepest changed signal that does).
//
// Pure glib (+ libfst), no GTK, so it can be unit-tested directly.

typedef struct _EventSeqChange EventSeqChange;

struct _EventSeqChange
{
    gchar *path; // dot-delimited full hierarchical name, no bit-range suffix;
                 // for a net with several names (VCD id-code / FST handle
                 // aliases) the deepest-scoped one
    GPtrArray *aliases; // gchar*: the net's other names, empty if none
    gchar *value; // as dumped: "1", "b1010", "r3.5", "sfoo"
    guint seq; // 1-based position in the dump file's order at this
               // timestamp, 0 if unknown (FST)
    gint level; // causal level, -1 if causality hasn't been computed
    GPtrArray *causes; // EventSeqChange* (borrowed, same result) whose change
                       // feeds this one; empty if none
    guint index; // position in result->changes, set by assign_causality()
    gboolean dumped_before_cause; // TRUE if some cause appears LATER in the
                                  // dump order than this change (VCD only):
                                  // dump order contradicts causality
};

typedef struct
{
    gint64 time;
    gboolean order_known;
    gboolean causality_known;
    GPtrArray *changes; // EventSeqChange*, in dump order when order_known
    gchar *warning; // NULL, or a one-line caveat to show the user
} EventSeqResult;

typedef enum
{
    EVENT_SEQUENCE_ERROR_UNSUPPORTED,
    EVENT_SEQUENCE_ERROR_IO,
    EVENT_SEQUENCE_ERROR_PARSE,
} EventSequenceError;

GQuark event_sequence_error_quark(void);
#define EVENT_SEQUENCE_ERROR (event_sequence_error_quark())

// Scans dump_path (VCD, optionally .gz, or FST -- chosen by content, not
// extension) for every value change at exactly `time`, in raw dump time
// units. Returns NULL with error set on failure. A timestamp with no
// changes is a valid, empty result.
EventSeqResult *event_sequence_scan(const gchar *dump_path, gint64 time, GError **error);

// Returns the fan-in signal paths (array of gchar*, free func g_free) for
// `path`, or NULL if unknown/unqueryable. Only the paths that also changed
// at this timestamp matter, so extras are fine.
typedef GPtrArray *(*EventSeqFaninFunc)(const gchar *path, gpointer user_data);

// Fills in level/causes/dumped_before_cause on every change using
// `fanin`, and sets result->causality_known. `max_queries` caps how many
// signals are looked up (a netlist query spawns a process each); signals
// beyond the cap are left at level -1 and a warning is set. 0 = no cap.
void event_sequence_assign_causality(EventSeqResult *result,
                                     EventSeqFaninFunc fanin,
                                     gpointer user_data,
                                     guint max_queries);

void event_sequence_result_free(EventSeqResult *result);

// Returns `name` without a trailing Verilog bit-range ("bus [7:0]" ->
// "bus", "q[3]" -> "q"), newly allocated. Used to make dump names and
// gtkwave trace names comparable with netlist paths.
gchar *event_sequence_strip_range(const gchar *name);

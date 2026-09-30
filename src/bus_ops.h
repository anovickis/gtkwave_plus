#pragma once

#include <config.h>
#include <gtkwave.h>

// Bus operations: the "Create/Edit/Adjust/Reverse Bus, expand-as-sub-bus"
// TODO item. "Create Bus" (combining several 1-bit signals into one bus)
// already existed as Edit > Combine Down/Up (menu.c's combine_traces());
// this file adds what didn't: slicing a contiguous bit range out of an
// EXISTING bus into its own narrower bus trace, and a one-click bit-order
// reversal (a thin wrapper around the existing TR_REVERSE flag/dataformat()
// machinery, which already drives display/decoding -- see baseconvert.c).

// Opens a small modal dialog to pick a bit-index range (bounded to `t`'s
// own current bit count) and, on confirmation, builds a new vector trace
// from just that slice and adds it to the display. No-ops with a status
// message if `t` isn't a vector/bus trace.
void bus_ops_show_expand_subbus_dialog(GwTrace *t);

// Toggles TR_REVERSE on `t` (a vector/bus trace) and redraws. No-ops with
// a status message if `t` isn't a vector/bus trace.
void bus_ops_reverse_bit_order(GwTrace *t);

// Opens a dialog to reorder, remove, and add constituent bits of an
// existing bus, then build a new bus trace with that composition (the
// original bus is left untouched -- same non-destructive precedent as
// Expand-as-Sub-Bus above). No-ops with a status message if `t` isn't a
// bus (multi-bit) signal.
void bus_ops_show_edit_composition_dialog(GwTrace *t);

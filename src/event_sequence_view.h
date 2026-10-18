#pragma once

// Event Sequence pane: for one timestamp (defaults to the primary
// marker), lists every value change in the order it appears in the dump
// (VCD) and, if the design's netlist is configured, ranks the changes by
// causal level using each signal's fan-in cone (see event_sequence.h for
// the reasoning and limits). Opens a window; no-op with a status message
// if the dump can't be re-scanned.
void event_sequence_view_show(void);

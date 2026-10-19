#pragma once

// "Trace Cause" window: explains why a signal changed at (or most recently
// before) the primary marker, as a tree you expand to walk back through the
// design towards the stimulus (see cause_trace.h for how causes are
// ranked and what the dump cannot tell). `dump_path` is the signal's full
// hierarchical name as the dump shows it. Needs the design's HDL sources
// ([[Configuring HDL Sources]]); a status message explains if it can't open.
void cause_trace_view_show(const char *dump_path);

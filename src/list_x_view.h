#pragma once

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

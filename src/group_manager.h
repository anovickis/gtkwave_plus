#pragma once

#include <config.h>
#include <gtkwave.h>

// Group Manager: a real dialog over the existing group data model
// (begin/end TR_GRP_BEGIN/TR_GRP_END sentinel pairs spliced into
// GLOBALS->traces, see menu.c's create_group()) rather than a new one.
// Lists every group currently in the trace list, indented by nesting
// depth, with per-group actions this codebase didn't have an entry point
// for before: bulk "move the currently-highlighted signals into this
// group" (there was previously no way to add already-existing traces to
// an already-existing group -- create_group() only ever builds a brand
// new group from the current highlight), Rename, and Ungroup (remove the
// group markers, keep its contents in place).
void group_manager_show(void);

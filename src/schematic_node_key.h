#pragma once

#include <glib.h>

// Stable identities for the nodes of a slang-netlist cone DOT graph.
//
// slang-netlist numbers its nodes ("N26") differently on every run -- the
// same cone queried twice comes back with different ids for every internal
// node -- so an id can't be used to remember anything about a node, such as
// where the user dragged it. What IS stable is the graph's structure: a
// node's label plus what feeds it and what it feeds, recursively.
//
// Each node's key is computed by iterative neighbourhood refinement
// (Weisfeiler-Lehman colour refinement): start from the node's label, then
// repeatedly fold in the sorted (direction, edge label, neighbour colour)
// triples of its edges until the number of distinct colours stops growing.
// Run on the FULL cone DOT (not the depth-pruned one), so a key doesn't
// change when the user changes the depth limit.
//
// Two nodes can still end up with the SAME key when they're structurally
// indistinguishable (e.g. two identical parallel branches). Callers that
// need unique keys must break such ties themselves.

// dot: slang-netlist's cone DOT. Returns node id (e.g. "N26") -> newly
// allocated key string (hex). Never NULL; empty for unparseable input.
// Free with g_hash_table_unref().
GHashTable *schematic_node_keys(const gchar *dot);

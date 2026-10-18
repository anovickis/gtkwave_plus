#pragma once

#include <glib.h>

#include "struct_scope.h"

// "Add as Struct": shows a struct / union / VHDL-record scope of the dump as
// ONE expandable item in the waveform instead of a pile of loose signals.
//
// A struct becomes a group with its fields in declaration order inside it;
// nested structs and unions become nested groups. With `packed_value`, a
// struct's group header is also a real bus carrying the packed value (first
// field in the most significant bits), so collapsed it shows the whole
// struct's value and expanded it shows the fields -- the same shape the
// existing Expand command gives a bus. A union's fields overlay each other,
// so a union is always a plain named group; where a union sits inside a
// packed struct it contributes the bits of its widest member.
//
// Neither FST nor VCD records whether a struct was PACKED, and an unpacked
// struct has no meaningful single value, so "packed value" is the user's
// choice rather than something decided here.
//
// `scope_path` is the scope's full dotted path in the loaded dump, `kind`
// what the dump says it is (STRUCT_MEMBER_STRUCT / _UNION / _RECORD).
// `mode` is WV_RECURSE_APPEND / _INSERT / _REPLACE (see treesearch.h).
// Reports problems through the status line and *error.
gboolean struct_group_add(const gchar *scope_path,
                          StructMemberKind kind,
                          guint mode,
                          gboolean packed_value,
                          GError **error);

// Maps the SST's tree-node kind (a GwTreeKind value) to a struct-scope
// kind, or returns FALSE if the node isn't a struct-like scope.
gboolean struct_group_kind_from_tree_kind(guint tree_kind, StructMemberKind *out);

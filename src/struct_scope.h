#pragma once

#include <glib.h>

// Declaration-order contents of one scope of a dump file, for building a
// struct (a group of its fields, optionally with the packed aggregate value).
//
// gtkwave itself can't supply this: its in-memory hierarchy is sorted
// alphabetically, so the order the fields were declared in -- which is the
// bit order of a packed struct, first field in the most significant bits --
// is gone once the dump is loaded. The dump's header still has it, so it's
// re-read for the one scope that's wanted (FST, or VCD optionally gzipped).

typedef enum
{
    STRUCT_MEMBER_VAR, // a signal
    STRUCT_MEMBER_STRUCT, // a nested struct scope
    STRUCT_MEMBER_UNION, // a nested union scope
    STRUCT_MEMBER_RECORD, // a nested VHDL record scope
    STRUCT_MEMBER_SCOPE, // any other nested scope (module, block, ...)
} StructMemberKind;

typedef struct
{
    gchar *name; // as declared, without a trailing bit range
    StructMemberKind kind;
    guint width; // signals only; 0 for scopes
} StructMember;

typedef enum
{
    STRUCT_SCOPE_ERROR_UNSUPPORTED,
    STRUCT_SCOPE_ERROR_IO,
    STRUCT_SCOPE_ERROR_NOT_FOUND,
} StructScopeError;

GQuark struct_scope_error_quark(void);
#define STRUCT_SCOPE_ERROR (struct_scope_error_quark())

// The immediate children of the scope whose full dotted path is
// `scope_path` (e.g. "tb.dut.instr"), in the order they were declared.
// Returns a GPtrArray of StructMember* (free with g_ptr_array_unref()), or
// NULL with *error set. A scope that exists but is empty is an empty array;
// one that doesn't exist is NOT_FOUND.
GPtrArray *struct_scope_members(const gchar *dump_path, const gchar *scope_path, GError **error);

void struct_member_free(StructMember *member);

// TRUE for the kinds that are struct-like containers a user can "Add as
// Struct": struct, union, record.
gboolean struct_member_kind_is_container(StructMemberKind kind);

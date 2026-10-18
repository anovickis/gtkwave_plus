#pragma once

#include <glib.h>

// A user-written field layout for a bus whose dump carries no struct
// information (a packed struct flattened to one wide vector):
//
//   op:4 imm:8 valid:1            sequential: widths, first field in the MOST
//                                 significant bits, running downward
//   op[12:9] imm[8:1] valid[0]    explicit bit positions (bit 0 = least
//                                 significant bit of the bus)
//
// Fields are separated by spaces, commas or newlines. A field named `_` is
// padding: it takes up bits but isn't shown. One spec uses one style, not a
// mix, since a sequential field's position depends on what came before it.

typedef struct
{
    gchar *name;
    guint msb, lsb; // bit numbers, counted from the bus's least significant bit
    gboolean padding; // `_`: reserved bits, not shown
} StructField;

typedef enum
{
    STRUCT_FIELDS_ERROR_SYNTAX,
    STRUCT_FIELDS_ERROR_RANGE,
} StructFieldsError;

GQuark struct_fields_error_quark(void);
#define STRUCT_FIELDS_ERROR (struct_fields_error_quark())

void struct_field_free(StructField *field);

// Parses `text` for a bus `bus_width` bits wide. Returns the fields most
// significant first (GPtrArray of StructField*, free with g_ptr_array_unref()),
// or NULL with *error describing the first problem (named, with its position).
// *unused_bits (may be NULL) is the count of bus bits no field covers --
// not an error, but worth telling the user about.
GPtrArray *struct_fields_parse(const gchar *text,
                               guint bus_width,
                               guint *unused_bits,
                               GError **error);

// One line per field for showing in a preview, e.g. "op  [12:9]  4 bits".
gchar *struct_fields_describe(const GPtrArray *fields);

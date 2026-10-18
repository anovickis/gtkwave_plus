#pragma once

#include <glib.h>

// Derived ("virtual") signals: a Verilog-flavoured expression over existing
// signals, evaluated across time into a new history that is then shown like
// any other signal. GTK-free -- operates on plain value series so it can be
// unit-tested without a dump file.
//
// Language (Verilog precedence, unsigned, 4-state):
//   operands   signal references (bare `top.u.count`, or "quoted name" for
//              odd characters, e.g. "top.bus[7:0]"), literals (`42`,
//              `8'hFF`, `4'b10x1`, `'d5`), `{a, b}` concatenation, `{4{a}}`
//              replication, bit-select `x[3]`, part-select `x[7:4]`
//   unary      ~  !  -   and reductions  &  |  ^  ~&  ~|  ~^
//   binary     * / %   + -   << >>   < <= > >=   == !=   &   ^ ^~   |   && ||
//   ternary    c ? a : b
//
// Widths are self-determined: +,-,*,/,%,&,|,^ and ?: take the WIDER operand
// (no carry-out bit, results wrap), shifts take the left operand's width,
// comparisons / logical ops / reductions give 1 bit, an unsized literal is
// just wide enough for its value. Arithmetic and comparisons are limited to
// 64-bit operands; bitwise ops, concatenation and selects are not.
//
// X/Z handling: bitwise and logical operators propagate unknowns bit by bit
// the Verilog way (0 & x = 0, 1 | x = 1); arithmetic, comparisons and shift
// amounts are pessimistic (any unknown operand bit makes the whole result X).

typedef struct _DerivedExpr DerivedExpr;

typedef enum
{
    DERIVED_EXPR_ERROR_SYNTAX,
    DERIVED_EXPR_ERROR_UNKNOWN_SIGNAL,
    DERIVED_EXPR_ERROR_WIDTH,
    DERIVED_EXPR_ERROR_UNSUPPORTED,
} DerivedExprError;

GQuark derived_expr_error_quark(void);
#define DERIVED_EXPR_ERROR (derived_expr_error_quark())

// Maps a name as the user typed it to the canonical signal name (a full
// hierarchical path, or an alias's target), or NULL if there is no such
// signal. Called while parsing, including for "name[range]" candidates, so
// it must be side-effect free. Returned string is newly allocated.
typedef gchar *(*DerivedNameResolver)(const gchar *name, gpointer user_data);

// Parses `text`. Every signal reference is passed through `resolve`
// (NULL = accept every name unchanged); an unresolved one is an
// UNKNOWN_SIGNAL error naming it.
DerivedExpr *derived_expr_parse(const gchar *text,
                                DerivedNameResolver resolve,
                                gpointer user_data,
                                GError **error);
void derived_expr_free(DerivedExpr *expr);

// Distinct canonical signal names the expression reads, in order of first
// appearance (gchar*, borrowed from `expr`).
GPtrArray *derived_expr_references(const DerivedExpr *expr);

// Canonical, fully parenthesised text that parses back to an equivalent
// expression (names quoted whenever they aren't plain identifiers). This is
// what gets saved, so it never depends on the aliases used to type it.
gchar *derived_expr_to_string(const DerivedExpr *expr);

/* ------------------------------------------------------------- series -- */

typedef enum
{
    DERIVED_BIT_0 = 0,
    DERIVED_BIT_1 = 1,
    DERIVED_BIT_X = 2,
    DERIVED_BIT_Z = 3,
} DerivedBit;

// A signal's history as changes: at times[i] the value becomes bits[i *
// width ...], MSB first. Before times[0] the value is unknown (X).
typedef struct
{
    guint width;
    guint n;
    guint capacity; // allocated entries (managed by derived_series_append())
    gint64 *times; // strictly ascending
    guint8 *bits; // n * width DerivedBit values
} DerivedSeries;

DerivedSeries *derived_series_new(guint width);
void derived_series_append(DerivedSeries *series, gint64 time, const guint8 *bits);
void derived_series_free(DerivedSeries *series);

typedef struct
{
    const gchar *name; // canonical, as derived_expr_references() lists it
    const DerivedSeries *series;
} DerivedInput;

// Result width for the given input widths, or FALSE with *error set
// (WIDTH for arithmetic wider than 64 bits, mismatched shapes).
gboolean derived_expr_width(const DerivedExpr *expr,
                            const DerivedInput *inputs,
                            guint n_inputs,
                            guint *out_width,
                            GError **error);

// Evaluates the expression at every time any input changes and returns the
// resulting history (consecutive equal values collapsed). `inputs` must
// cover derived_expr_references(). NULL with *error on failure.
DerivedSeries *derived_expr_evaluate(const DerivedExpr *expr,
                                     const DerivedInput *inputs,
                                     guint n_inputs,
                                     GError **error);

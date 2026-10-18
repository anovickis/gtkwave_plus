#include "struct_fields.h"

#include <ctype.h>
#include <string.h>

G_DEFINE_QUARK(struct-fields-error-quark, struct_fields_error)

void struct_field_free(StructField *field)
{
    if (field == NULL) {
        return;
    }
    g_free(field->name);
    g_free(field);
}

static gboolean is_name_start(gchar c)
{
    return isalpha((guchar)c) || c == '_';
}

static gboolean is_name_char(gchar c)
{
    return isalnum((guchar)c) || c == '_';
}

#define FAIL(code, ...) \
    G_STMT_START \
    { \
        g_set_error(error, STRUCT_FIELDS_ERROR, code, __VA_ARGS__); \
        goto fail; \
    } \
    G_STMT_END

// Like FAIL, but also releases `nm` -- after the message has been formatted,
// since the message may quote it.
#define FAIL_FREEING(nm, code, ...) \
    G_STMT_START \
    { \
        g_set_error(error, STRUCT_FIELDS_ERROR, code, __VA_ARGS__); \
        g_free(nm); \
        goto fail; \
    } \
    G_STMT_END

// Reads a non-negative decimal number at *p, advancing it.
static gboolean read_uint(const gchar **p, guint *out)
{
    if (!isdigit((guchar)**p)) {
        return FALSE;
    }
    guint64 v = 0;
    while (isdigit((guchar)**p)) {
        v = v * 10 + (guint64)(**p - '0');
        if (v > 1000000) {
            return FALSE;
        }
        (*p)++;
    }
    *out = (guint)v;
    return TRUE;
}

static gint cmp_msb_desc(gconstpointer a, gconstpointer b)
{
    const StructField *x = *(StructField *const *)a;
    const StructField *y = *(StructField *const *)b;
    return x->msb == y->msb ? 0 : (x->msb > y->msb ? -1 : 1);
}

GPtrArray *struct_fields_parse(const gchar *text, guint bus_width, guint *unused_bits, GError **error)
{
    GPtrArray *fields = g_ptr_array_new_with_free_func((GDestroyNotify)struct_field_free);
    GHashTable *seen = g_hash_table_new(g_str_hash, g_str_equal);
    gboolean sequential = FALSE, explicit_style = FALSE;
    guint cursor = bus_width; // next sequential field's msb + 1
    const gchar *p = text != NULL ? text : "";

    if (bus_width == 0) {
        FAIL(STRUCT_FIELDS_ERROR_RANGE, "The signal has no bits to divide into fields.");
    }

    for (;;) {
        while (*p != '\0' && (isspace((guchar)*p) || *p == ',')) {
            p++;
        }
        if (*p == '\0') {
            break;
        }

        const gchar *start = p;
        if (!is_name_start(*p)) {
            FAIL(STRUCT_FIELDS_ERROR_SYNTAX, "Expected a field name at \"%.12s\".", p);
        }
        while (is_name_char(*p)) {
            p++;
        }
        gchar *name = g_strndup(start, (gsize)(p - start));
        gboolean padding = g_str_equal(name, "_");

        guint msb = 0, lsb = 0;
        if (*p == ':') {
            p++;
            guint width;
            if (!read_uint(&p, &width) || width == 0) {
                FAIL_FREEING(name, STRUCT_FIELDS_ERROR_SYNTAX, "Field '%.*s' needs a width of at least 1 after ':'.",
                     (gint)(p - start), start);
            }
            if (explicit_style) {
                FAIL_FREEING(name, STRUCT_FIELDS_ERROR_SYNTAX,
                     "Field '%s' uses a width but earlier fields use bit ranges; use one style.", name);
            }
            sequential = TRUE;
            if (width > cursor) {
                guint total = 0;
                for (guint i = 0; i < fields->len; i++) {
                    const StructField *f = g_ptr_array_index(fields, i);
                    total += f->msb - f->lsb + 1;
                }
                FAIL_FREEING(name, STRUCT_FIELDS_ERROR_RANGE,
                     "The fields add up to %u bits but the signal is only %u wide.",
                     total + width, bus_width);
            }
            msb = cursor - 1;
            lsb = cursor - width;
            cursor = lsb;
        } else if (*p == '[') {
            p++;
            if (!read_uint(&p, &msb)) {
                FAIL_FREEING(name, STRUCT_FIELDS_ERROR_SYNTAX, "Expected a bit number after '[' in field '%.*s'.",
                     (gint)(p - start), start);
            }
            lsb = msb;
            if (*p == ':') {
                p++;
                if (!read_uint(&p, &lsb)) {
                FAIL_FREEING(name, STRUCT_FIELDS_ERROR_SYNTAX, "Expected a bit number after ':' in field '%s'.",
                         name);
                }
            }
            if (*p != ']') {
                FAIL_FREEING(name, STRUCT_FIELDS_ERROR_SYNTAX, "Expected ']' to close the bit range of field.");
            }
            p++;
            if (sequential) {
                FAIL_FREEING(name, STRUCT_FIELDS_ERROR_SYNTAX,
                     "A bit range follows fields that use widths; use one style.");
            }
            explicit_style = TRUE;
            if (lsb > msb) {
                FAIL_FREEING(name, STRUCT_FIELDS_ERROR_RANGE, "Bit range [%u:%u] has its bounds reversed.", msb, lsb);
            }
            if (msb >= bus_width) {
                guint hi = msb;
                FAIL_FREEING(name, STRUCT_FIELDS_ERROR_RANGE,
                     "Bit %u is outside the signal (it has bits 0 to %u).", hi, bus_width - 1);
            }
        } else {
            guint here = (guint)(p - start);
            gchar *shown = g_strndup(start, here);
            g_free(name);
            g_set_error(error, STRUCT_FIELDS_ERROR, STRUCT_FIELDS_ERROR_SYNTAX,
                        "Field '%s' needs a width (name:4) or a bit range (name[7:4]).", shown);
            g_free(shown);
            goto fail;
        }

        if (!padding) {
            if (g_hash_table_contains(seen, name)) {
                gchar *dup = g_strdup(name);
                g_free(name);
                g_set_error(error, STRUCT_FIELDS_ERROR, STRUCT_FIELDS_ERROR_SYNTAX,
                            "The field name '%s' is used twice.", dup);
                g_free(dup);
                goto fail;
            }
        }

        StructField *f = g_new0(StructField, 1);
        f->name = name;
        f->msb = msb;
        f->lsb = lsb;
        f->padding = padding;
        g_ptr_array_add(fields, f);
        if (!padding) {
            g_hash_table_add(seen, f->name);
        }
    }

    if (fields->len == 0) {
        FAIL(STRUCT_FIELDS_ERROR_SYNTAX, "Enter at least one field, e.g.  op:4 imm:8 valid:1");
    }

    if (explicit_style) {
        g_ptr_array_sort(fields, cmp_msb_desc);
        for (guint i = 1; i < fields->len; i++) {
            const StructField *hi = g_ptr_array_index(fields, i - 1);
            const StructField *lo = g_ptr_array_index(fields, i);
            if (lo->msb >= hi->lsb) {
                FAIL(STRUCT_FIELDS_ERROR_RANGE, "Fields '%s' and '%s' overlap at bit %u.", hi->name,
                     lo->name, lo->msb);
            }
        }
    }

    guint covered = 0;
    gboolean any_shown = FALSE;
    for (guint i = 0; i < fields->len; i++) {
        const StructField *f = g_ptr_array_index(fields, i);
        covered += f->msb - f->lsb + 1;
        any_shown = any_shown || !f->padding;
    }
    if (!any_shown) {
        FAIL(STRUCT_FIELDS_ERROR_SYNTAX, "Every field is padding (_); name at least one.");
    }
    if (unused_bits != NULL) {
        *unused_bits = bus_width - covered;
    }

    g_hash_table_unref(seen);
    return fields;

fail:
    g_hash_table_unref(seen);
    g_ptr_array_unref(fields);
    return NULL;
}

gchar *struct_fields_describe(const GPtrArray *fields)
{
    GString *s = g_string_new(NULL);
    for (guint i = 0; i < fields->len; i++) {
        const StructField *f = g_ptr_array_index(fields, i);
        guint w = f->msb - f->lsb + 1;
        if (i > 0) {
            g_string_append_c(s, '\n');
        }
        if (w == 1) {
            g_string_append_printf(s, "%s  [%u]  1 bit%s", f->name, f->msb, f->padding ? "  (padding)" : "");
        } else {
            g_string_append_printf(s, "%s  [%u:%u]  %u bits%s", f->name, f->msb, f->lsb, w,
                                   f->padding ? "  (padding)" : "");
        }
    }
    return g_string_free(s, FALSE);
}

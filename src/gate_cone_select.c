#include "gate_cone_select.h"

#include <stdio.h>
#include <string.h>

GateExpansion *gate_expansion_new(const gchar *seed, guint hops)
{
    GateExpansion *e = g_new0(GateExpansion, 1);
    e->seed = g_strdup(seed);
    e->hops = hops;
    return e;
}

void gate_expansion_free(GateExpansion *expansion)
{
    if (expansion == NULL) {
        return;
    }
    g_free(expansion->seed);
    g_free(expansion);
}

static void append_hops(GString *s, guint hops)
{
    if (hops > 0) {
        g_string_append_printf(s, " %%ci%u", hops);
    } else {
        g_string_append(s, " %ci*");
    }
}

gchar *gate_cone_select_expression(const gchar *root_select,
                                   guint root_depth,
                                   GPtrArray *expansions,
                                   GPtrArray *filters)
{
    GString *s = g_string_new(root_select);
    append_hops(s, root_depth);

    guint n_exp = expansions != NULL ? expansions->len : 0;
    for (guint i = 0; i < n_exp; i++) {
        const GateExpansion *e = g_ptr_array_index(expansions, i);
        g_string_append_c(s, ' ');
        g_string_append(s, e->seed);
        // An expansion always has a bounded hop count; 0 would mean %ci*
        // and silently pull in the whole upstream design.
        append_hops(s, MAX(e->hops, 1u));
    }
    // N+1 sets on the stack (root + each expansion) fold with N unions.
    for (guint i = 0; i < n_exp; i++) {
        g_string_append(s, " %u");
    }

    // Yosys's select stack machine unions whatever is left when the
    // command ends, so N filtered objects need (N-1) %u to become ONE set
    // before a single %d subtracts it; a bare run of objects before one %d
    // subtracts nothing (confirmed against a real Yosys).
    guint n_filter = filters != NULL ? filters->len : 0;
    if (n_filter > 0) {
        for (guint i = 0; i < n_filter; i++) {
            g_string_append_c(s, ' ');
            g_string_append(s, g_ptr_array_index(filters, i));
        }
        for (guint i = 1; i < n_filter; i++) {
            g_string_append(s, " %u");
        }
        g_string_append(s, " %d");
    }
    return g_string_free(s, FALSE);
}

gchar *gate_cone_probe_script(GPtrArray *expansions)
{
    GString *s = g_string_new("");
    if (expansions != NULL) {
        for (guint i = 0; i < expansions->len; i++) {
            const GateExpansion *e = g_ptr_array_index(expansions, i);
            g_string_append_printf(s, "log GTKWAVE_PROBE %s; select -count %s; ", e->seed, e->seed);
        }
    }
    return g_string_free(s, FALSE);
}

GPtrArray *gate_cone_missing_seeds(const gchar *yosys_log, GPtrArray *expansions)
{
    GPtrArray *missing = g_ptr_array_new_with_free_func(g_free);
    if (yosys_log == NULL || expansions == NULL) {
        return missing;
    }

    gchar **lines = g_strsplit(yosys_log, "\n", -1);
    for (guint i = 0; lines[i] != NULL; i++) {
        const gchar *marker = strstr(lines[i], "GTKWAVE_PROBE ");
        if (marker == NULL) {
            continue;
        }
        const gchar *seed = marker + strlen("GTKWAVE_PROBE ");

        // The count is the next "<n> objects." line (Yosys may log other
        // things in between, e.g. the command echo).
        for (guint j = i + 1; lines[j] != NULL && j < i + 6; j++) {
            guint64 count;
            gchar tail[16];
            if (sscanf(lines[j], "%" G_GUINT64_FORMAT " %15s", &count, tail) == 2 &&
                g_str_has_prefix(tail, "object")) {
                if (count == 0) {
                    gchar *s = g_strstrip(g_strdup(seed));
                    g_ptr_array_add(missing, s);
                }
                break;
            }
        }
    }
    g_strfreev(lines);
    return missing;
}

gboolean gate_cone_name_is_unstable(const gchar *name)
{
    return name != NULL && strstr(name, "$auto$") != NULL;
}

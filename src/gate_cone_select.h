#pragma once

#include <glib.h>

// Builds the Yosys `select` command behind the gate-level cone window
// (gate_schematic_view.c) from the pieces the user has accumulated, so the
// logic can be tested against a real Yosys without any GTK.
//
// The shown cone is a UNION of seeds, each grown a bounded number of hops
// upstream:
//
//   select <root> %ci<depth>                  the queried signal, "start small"
//          <net1> %ci<h1> <net2> %ci<h2> ...  one entry per net the user has
//                                             expanded (a boundary port clicked)
//          %u %u ...                          fold them into one set
//          <obj> <obj> ... %u... %d           subtract anything filtered out
//
// Seeds are NET names, deliberately never cell names: a cell created by a
// pass that runs after `flatten` (every gate from opt/techmap) is named
// from a global counter, so dissolving an instance renumbers them
// (`$auto$ff.cc:337:slice$227` became `...$228` once u_alu was flattened,
// checked against a real Yosys), while user-declared wire names survive.
// Even so, anonymous names containing "$auto$" can shift; see
// gate_cone_name_is_unstable().

typedef struct
{
    gchar *seed; // full select object, "<top-module>/<net>"
    guint hops; // %ci<hops>; one hop from a net pulls in just its driver
} GateExpansion;

GateExpansion *gate_expansion_new(const gchar *seed, guint hops);
void gate_expansion_free(GateExpansion *expansion);

// Everything after the word `select`: "<root> %ci<depth> ...". `depth` 0
// means unlimited (%ci*). `expansions` (GateExpansion*) and `filters`
// (gchar* select objects) may be NULL.
gchar *gate_cone_select_expression(const gchar *root_select,
                                   guint root_depth,
                                   GPtrArray *expansions,
                                   GPtrArray *filters);

// Script fragment, to run just before the final `select`, that logs
// "GTKWAVE_PROBE <seed>" followed by Yosys's "<n> objects." for each
// expansion -- `select -count` doesn't disturb the current selection.
// "" if there are no expansions.
gchar *gate_cone_probe_script(GPtrArray *expansions);

// Seeds (gchar*, newly allocated) whose probe in `yosys_log` found 0
// objects, i.e. a net that no longer exists (renamed by optimization, or
// inside an instance that has since been collapsed). A seed with no probe
// result at all is NOT reported missing.
GPtrArray *gate_cone_missing_seeds(const gchar *yosys_log, GPtrArray *expansions);

// TRUE for a name that embeds a pass-generated counter ("$auto$"), which
// can renumber when the flatten set changes and so may silently start
// naming a DIFFERENT object. Callers drop such saved names on a flatten
// change rather than risk filtering or expanding the wrong thing.
gboolean gate_cone_name_is_unstable(const gchar *name);

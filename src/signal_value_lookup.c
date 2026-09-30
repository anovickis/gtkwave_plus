#include "signal_value_lookup.h"
#include "globals.h"
#include "gw-mouseover.h"
#include "debug.h"
#include <string.h>

// Populates node->harray/numhist -- the bsearch index bsearch_node() (used
// deep inside local_trace_asciival()) requires and otherwise segfaults
// without (bsearch() over a NULL base with a nonzero numhist). Normally
// only built lazily the first time a node becomes a real, visible trace
// (see the identical inlined block in AddNodeTraceReturn(),
// src/analyzer.c) -- harmless/idempotent to build here too for a node
// that never becomes one, which is exactly the common case for a lookup
// like this: the whole point is showing values for signals the user
// *isn't* already watching in the wave view.
static void ensure_harray(GwNode *node)
{
    if (node->harray != NULL) {
        return;
    }

    int histcount = 0;
    for (GwHistEnt *h = &node->head; h != NULL; h = h->next) {
        histcount++;
    }
    node->numhist = histcount;

    GwHistEnt **harray = malloc_2((gsize)histcount * sizeof(GwHistEnt *));
    node->harray = harray;

    GwHistEnt *h = &node->head;
    for (int i = 0; i < histcount; i++) {
        harray[i] = h;
        h = h->next;
    }
}

gchar *signal_value_at_time(const gchar *signal_path, GwTime time)
{
    if (GLOBALS->dump_file == NULL) {
        return NULL;
    }

    GwSymbol *sym = gw_dump_file_lookup_symbol(GLOBALS->dump_file, signal_path);
    if (sym == NULL || sym->n == NULL) {
        return NULL;
    }

    ensure_harray(sym->n);

    // A throwaway, stack-only trace borrowing the symbol's node -- mirrors
    // the pattern AddNodeTraceReturn() uses for a plain (non-vector,
    // non-aliased) symbol lookup, without registering a real, visible
    // trace in GLOBALS->traces.
    GwTrace temp;
    memset(&temp, 0, sizeof(GwTrace));
    temp.name = sym->name;
    temp.name_full = sym->name;
    temp.n.nd = sym->n;

    return local_trace_asciival(&temp, time);
}
